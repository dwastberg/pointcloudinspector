#include <pci/adapters/ogr/OgrVectorLoader.h>

#include <pci/adapters/gdal/runtime/GdalRuntime.h>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace pci {
namespace {

using GeometryPtr =
    std::unique_ptr<OGRGeometry,
                    decltype(&OGRGeometryFactory::destroyGeometry)>;

[[nodiscard]] GdalDatasetPtr openDataset(const std::filesystem::path &path)
{
    GdalDatasetOpenResult opened =
        tryOpenGdalDataset(path, GdalDatasetKind::Vector);
    if (!opened.dataset) {
        throw VectorImportError(
            gdalOpenFailureMessage("vector", path, opened.error));
    }
    return std::move(opened.dataset);
}

[[nodiscard]] std::string exportWkt(const OGRSpatialReference *srs)
{
    if (srs == nullptr) {
        return {};
    }
    char *wkt = nullptr;
    if (srs->exportToWkt(&wkt) != OGRERR_NONE || wkt == nullptr) {
        return {};
    }
    std::string result(wkt);
    CPLFree(wkt);
    return result;
}

[[nodiscard]] std::optional<Bounds3d> layerExtent(OGRLayer *layer)
{
    OGREnvelope envelope{};
    if (layer == nullptr || layer->GetExtent(&envelope, FALSE) != OGRERR_NONE) {
        return std::nullopt;
    }
    return Bounds3d{
        .minimum = {envelope.MinX, envelope.MinY, 0.0},
        .maximum = {envelope.MaxX, envelope.MaxY, 0.0},
    };
}

[[nodiscard]] std::optional<VectorGeometryKind>
kindFor(const OGRwkbGeometryType type) noexcept
{
    switch (wkbFlatten(type)) {
    case wkbPoint:
    case wkbMultiPoint:
        return VectorGeometryKind::Point;
    case wkbLineString:
    case wkbLinearRing:
    case wkbCircularString:
    case wkbCompoundCurve:
    case wkbMultiLineString:
    case wkbMultiCurve:
        return VectorGeometryKind::Line;
    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
    case wkbMultiPolygon:
    case wkbMultiSurface:
        return VectorGeometryKind::Polygon;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool xyDisjoint(const Bounds3d &left,
                              const Bounds3d &right) noexcept
{
    return left.maximum[0] < right.minimum[0] ||
           right.maximum[0] < left.minimum[0] ||
           left.maximum[1] < right.minimum[1] ||
           right.maximum[1] < left.minimum[1];
}

void checkCancelled(std::stop_token token);

void checkCurvePointLimit(const OGRGeometry *geometry,
                          const std::uint64_t maximum,
                          const char *limitName,
                          const std::stop_token token,
                          const int depth = 0)
{
    checkCancelled(token);
    if (geometry == nullptr || depth > 8)
        return;
    if (const auto *curve = dynamic_cast<const OGRSimpleCurve *>(geometry);
        curve != nullptr &&
        static_cast<std::uint64_t>(curve->getNumPoints()) > maximum) {
        throw VectorImportLimitExceeded(
            limitName, static_cast<std::uint64_t>(curve->getNumPoints()));
    }
    if (const auto *polygon = dynamic_cast<const OGRCurvePolygon *>(geometry);
        polygon != nullptr) {
        checkCurvePointLimit(polygon->getExteriorRingCurve(),
                             maximum,
                             limitName,
                             token,
                             depth + 1);
        for (int index = 0; index < polygon->getNumInteriorRings(); ++index) {
            checkCurvePointLimit(polygon->getInteriorRingCurve(index),
                                 maximum,
                                 limitName,
                                 token,
                                 depth + 1);
        }
        return;
    }
    const auto *collection =
        dynamic_cast<const OGRGeometryCollection *>(geometry);
    if (collection == nullptr)
        return;
    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        checkCurvePointLimit(collection->getGeometryRef(index),
                             maximum,
                             limitName,
                             token,
                             depth + 1);
    }
}

[[nodiscard]] VectorRing ringFrom(const OGRSimpleCurve *ring,
                                  const std::stop_token token)
{
    VectorRing result;
    if (ring == nullptr) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(ring->getNumPoints()));
    for (int index = 0; index < ring->getNumPoints(); ++index) {
        checkCancelled(token);
        result.push_back({ring->getX(index), ring->getY(index)});
    }
    return result;
}

void checkCancelled(const std::stop_token token)
{
    if (token.stop_requested()) {
        throw VectorImportCancelled("Vector import cancelled");
    }
}

[[nodiscard]] bool appendGeometry(const OGRGeometry *geometry,
                                  VectorGeometryBuilder &builder,
                                  const std::stop_token token,
                                  const int depth = 0)
{
    checkCancelled(token);
    if (geometry == nullptr || geometry->IsEmpty()) {
        builder.markSkippedPart();
        return false;
    }
    if (depth > 8) {
        builder.markSkippedPart();
        return false;
    }
    if (geometry->Is3D()) {
        builder.markSourceHadZ();
    }
    if (geometry->hasCurveGeometry(TRUE)) {
        checkCurvePointLimit(geometry,
                             builder.limits().maximumCurveControlPoints,
                             "maximumCurveControlPoints",
                             token);
        GeometryPtr linear(geometry->getLinearGeometry(
                               builder.limits().curveMaximumAngleStepDegrees),
                           OGRGeometryFactory::destroyGeometry);
        if (!linear) {
            builder.markSkippedPart();
            return false;
        }
        checkCurvePointLimit(linear.get(),
                             builder.limits().maximumLinearizedCurvePoints,
                             "maximumLinearizedCurvePoints",
                             token);
        return appendGeometry(linear.get(), builder, token, depth + 1);
    }

    switch (wkbFlatten(geometry->getGeometryType())) {
    case wkbPoint: {
        const auto *point = geometry->toPoint();
        return point != nullptr &&
               builder.addPoint(point->getX(), point->getY());
    }
    case wkbLineString:
    case wkbLinearRing: {
        const auto *curve = dynamic_cast<const OGRSimpleCurve *>(geometry);
        if (curve == nullptr) {
            builder.markSkippedPart();
            return false;
        }
        const VectorRing ring = ringFrom(curve, token);
        return builder.addLineString(
            ring, wkbFlatten(geometry->getGeometryType()) == wkbLinearRing);
    }
    case wkbPolygon:
    case wkbTriangle: {
        const auto *polygon = dynamic_cast<const OGRPolygon *>(geometry);
        if (polygon == nullptr || polygon->getExteriorRing() == nullptr) {
            builder.markSkippedPart();
            return false;
        }
        std::vector<VectorRing> rings;
        rings.reserve(static_cast<std::size_t>(polygon->getNumInteriorRings()) +
                      1U);
        rings.push_back(ringFrom(polygon->getExteriorRing(), token));
        for (int index = 0; index < polygon->getNumInteriorRings(); ++index) {
            checkCancelled(token);
            rings.push_back(ringFrom(polygon->getInteriorRing(index), token));
        }
        return builder.addPolygon(rings);
    }
    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbGeometryCollection: {
        const auto *collection =
            dynamic_cast<const OGRGeometryCollection *>(geometry);
        if (collection == nullptr) {
            builder.markSkippedPart();
            return false;
        }
        bool any = false;
        for (int index = 0; index < collection->getNumGeometries(); ++index) {
            any = appendGeometry(collection->getGeometryRef(index),
                                 builder,
                                 token,
                                 depth + 1) ||
                  any;
        }
        return any;
    }
    default:
        builder.markSkippedPart();
        return false;
    }
}

[[nodiscard]] std::optional<std::array<double, 2>>
firstCoordinate(const OGRGeometry *geometry,
                const std::stop_token token,
                const int depth = 0)
{
    checkCancelled(token);
    if (geometry == nullptr || geometry->IsEmpty() || depth > 8) {
        return std::nullopt;
    }
    if (wkbFlatten(geometry->getGeometryType()) == wkbPoint) {
        const auto *point = dynamic_cast<const OGRPoint *>(geometry);
        if (point == nullptr) {
            return std::nullopt;
        }
        if (std::isfinite(point->getX()) && std::isfinite(point->getY())) {
            return std::array<double, 2>{point->getX(), point->getY()};
        }
    }
    if (const auto *curve = dynamic_cast<const OGRSimpleCurve *>(geometry)) {
        for (int index = 0; index < curve->getNumPoints(); ++index) {
            checkCancelled(token);
            if (std::isfinite(curve->getX(index)) &&
                std::isfinite(curve->getY(index))) {
                return std::array<double, 2>{curve->getX(index),
                                             curve->getY(index)};
            }
        }
    }
    const auto *collection =
        dynamic_cast<const OGRGeometryCollection *>(geometry);
    if (collection == nullptr) {
        return std::nullopt;
    }
    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        if (const auto result = firstCoordinate(
                collection->getGeometryRef(index), token, depth + 1)) {
            return result;
        }
    }
    return std::nullopt;
}

[[nodiscard]] OGRLayer *requestedLayer(GDALDataset *dataset,
                                       const VectorSublayerKey key)
{
    if (dataset == nullptr || key.index < 0 ||
        key.index >= dataset->GetLayerCount()) {
        throw VectorImportError("Requested vector sublayer is unavailable");
    }
    OGRLayer *layer = dataset->GetLayer(key.index);
    if (layer == nullptr) {
        throw VectorImportError("Requested vector sublayer is unavailable");
    }
    return layer;
}

} // namespace

VectorImportPreflight
OgrVectorLoader::inspect(const VectorImportRequest &request) const
{
    GdalDatasetPtr dataset = openDataset(request.sourcePath);
    VectorImportPreflight result;
    result.sourcePath = request.sourcePath;
    result.driverName = dataset->GetDriver() == nullptr
                            ? ""
                            : dataset->GetDriver()->GetDescription();
    for (int index = 0; index < dataset->GetLayerCount(); ++index) {
        OGRLayer *layer = dataset->GetLayer(index);
        if (layer == nullptr) {
            continue;
        }
        VectorSublayerInfo info;
        info.key = {.index = index,
                    .name =
                        layer->GetName() == nullptr ? "" : layer->GetName()};
        info.geometryTypeLabel = OGRGeometryTypeToName(layer->GetGeomType());
        info.spatialReferenceWkt = exportWkt(layer->GetSpatialRef());
        info.kind = kindFor(layer->GetGeomType());
        info.featureCount = layer->GetFeatureCount(FALSE);
        info.hasZ = wkbHasZ(layer->GetGeomType());
        info.extent = layerExtent(layer);
        result.sublayers.push_back(std::move(info));
    }
    return result;
}

std::array<double, 2> OgrVectorLoader::probeOrigin(
    const VectorImportRequest &request,
    const std::span<const VectorSublayerKey> sublayers) const
{
    GdalDatasetPtr dataset = openDataset(request.sourcePath);
    for (const VectorSublayerKey &key : sublayers) {
        OGRLayer *layer = requestedLayer(dataset.get(), key);
        layer->ResetReading();
        while (OGRFeature *raw = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw, OGRFeature::DestroyFeature);
            if (const auto result = firstCoordinate(feature->GetGeometryRef(),
                                                    request.stopToken)) {
                return *result;
            }
        }
    }
    throw VectorImportError(
        "Could not find a finite coordinate for vector layer origin");
}

VectorLayerDataPtr
OgrVectorLoader::loadSublayer(const VectorImportRequest &request,
                              VectorSublayerKey sublayer) const
{
    if (!validVectorImportLimits(request.limits)) {
        throw VectorImportError("Invalid vector import limits");
    }
    GdalDatasetPtr dataset = openDataset(request.sourcePath);
    OGRLayer *layer = requestedLayer(dataset.get(), sublayer);
    std::array<double, 2> originCoordinate{};
    if (request.origin) {
        originCoordinate = *request.origin;
    } else if (const auto extent = layerExtent(layer)) {
        originCoordinate = {extent->center()[0], extent->center()[1]};
    } else {
        const std::array<VectorSublayerKey, 1> keys{sublayer};
        originCoordinate = probeOrigin(request, keys);
    }
    VectorGeometryBuilder builder(
        vectorLayerOrigin(originCoordinate[0], originCoordinate[1]),
        request.limits);
    std::uint64_t featureCount = 0;
    layer->ResetReading();
    while (OGRFeature *raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(raw, OGRFeature::DestroyFeature);
        checkCancelled(request.stopToken);
        if (featureCount == request.limits.maximumSourceFeatures) {
            throw VectorImportLimitExceeded("maximumSourceFeatures",
                                            featureCount + 1);
        }
        ++featureCount;
        const OGRGeometry *geometry = feature->GetGeometryRef();
        if (geometry == nullptr || geometry->IsEmpty() ||
            !appendGeometry(geometry, builder, request.stopToken)) {
            builder.markSkippedFeature();
        }
        if (request.progress && featureCount % 8192U == 0U) {
            request.progress({.processed = featureCount, .total = 0});
        }
    }
    if (request.progress) {
        request.progress({.processed = featureCount, .total = featureCount});
    }
    auto data = std::make_shared<VectorLayerData>(std::move(builder).build());
    data->featureCount = featureCount;
    data->sourcePath = request.sourcePath;
    data->sourceDriver = dataset->GetDriver() == nullptr
                             ? ""
                             : dataset->GetDriver()->GetDescription();
    data->sublayerName = sublayer.name;
    data->spatialReferenceWkt = exportWkt(layer->GetSpatialRef());
    if (!request.targetSpatialReferenceWkt.empty() &&
        layer->GetSpatialRef() != nullptr) {
        OGRSpatialReference target;
        if (target.importFromWkt(request.targetSpatialReferenceWkt.c_str()) ==
            OGRERR_NONE) {
            data->crsMismatch = !target.IsSame(layer->GetSpatialRef());
        }
    }
    if (request.targetExtent && request.targetExtent->valid() &&
        data->bounds.valid()) {
        data->extentDisjointXY =
            xyDisjoint(data->bounds, *request.targetExtent);
    }
    return data;
}

} // namespace pci
