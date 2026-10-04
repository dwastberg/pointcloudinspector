#include <pci/vector/VectorGeometry.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/vector/VectorLayerData.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>

#include <mapbox/earcut.hpp>

namespace pci {
namespace {

[[nodiscard]] bool finite(const std::array<double, 2> value) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]);
}

[[nodiscard]] bool same(const std::array<double, 2> left,
                        const std::array<double, 2> right) noexcept
{
    return left[0] == right[0] && left[1] == right[1];
}

[[nodiscard]] double signedArea(const VectorRing &ring) noexcept
{
    if (ring.size() < 3) {
        return 0.0;
    }
    double result = 0.0;
    for (std::size_t index = 0; index < ring.size(); ++index) {
        const auto &from = ring[index];
        const auto &to = ring[(index + 1U) % ring.size()];
        result += from[0] * to[1] - to[0] * from[1];
    }
    return result * 0.5;
}

[[nodiscard]] double triangleArea(const VectorVertex2f first,
                                  const VectorVertex2f second,
                                  const VectorVertex2f third) noexcept
{
    const double cross =
        static_cast<double>(second.x - first.x) * (third.y - first.y) -
        static_cast<double>(second.y - first.y) * (third.x - first.x);
    return std::abs(cross) * 0.5;
}

[[nodiscard]] std::uint64_t checkedAddOrThrow(const std::uint64_t left,
                                              const std::uint64_t right,
                                              const char *limit)
{
    const auto result = pci::checkedAdd(left, right);
    if (!result) {
        throw VectorImportLimitExceeded(
            limit, std::numeric_limits<std::uint64_t>::max());
    }
    return *result;
}

[[nodiscard]] std::uint64_t checkedMultiplyOrThrow(const std::uint64_t left,
                                                   const std::uint64_t right,
                                                   const char *limit)
{
    const auto result = pci::checkedMultiply(left, right);
    if (!result) {
        throw VectorImportLimitExceeded(
            limit, std::numeric_limits<std::uint64_t>::max());
    }
    return *result;
}

} // namespace

VectorImportLimitExceeded::VectorImportLimitExceeded(const char *limit,
                                                     const std::uint64_t value)
    : VectorImportError([limit, value] {
        std::ostringstream result;
        result << "Vector import limit exceeded: " << limit << " (" << value
               << ')';
        return result.str();
    }())
{
}

Vec3d vectorLayerOrigin(const double x, const double y) noexcept
{
    return {
        .x = std::floor(x / 1024.0) * 1024.0,
        .y = std::floor(y / 1024.0) * 1024.0,
        .z = 0.0,
    };
}

bool validVectorImportLimits(const VectorImportLimits &limits) noexcept
{
    return limits.maximumApplicationWorkingBytes != 0 &&
           std::isfinite(limits.curveMaximumAngleStepDegrees) &&
           limits.curveMaximumAngleStepDegrees > 0.0 &&
           limits.curveMaximumAngleStepDegrees <= 90.0;
}

VectorGeometryKind VectorGeometrySummary::dominantKind() const noexcept
{
    if (polygonParts != 0) {
        return VectorGeometryKind::Polygon;
    }
    if (lineParts != 0) {
        return VectorGeometryKind::Line;
    }
    return VectorGeometryKind::Point;
}

VectorGeometryBuilder::VectorGeometryBuilder(const Vec3d origin,
                                             VectorImportLimits limits)
    : origin_(origin)
    , limits_(limits)
{
    if (!isFinite(origin_) || !validVectorImportLimits(limits_)) {
        throw std::invalid_argument(
            "Invalid vector geometry builder configuration");
    }
}

bool VectorGeometryBuilder::validCoordinate(const double x,
                                            const double y) const noexcept
{
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x - origin_.x) <= maximumOriginRelativeMetres &&
           std::abs(y - origin_.y) <= maximumOriginRelativeMetres;
}

void VectorGeometryBuilder::ensureLimit(const char *name,
                                        const std::uint64_t current,
                                        const std::uint64_t addition,
                                        const std::uint64_t limit) const
{
    const std::uint64_t next = checkedAddOrThrow(current, addition, name);
    if (next > limit) {
        throw VectorImportLimitExceeded(name, next);
    }
}

std::uint64_t VectorGeometryBuilder::retainedBytes() const noexcept
{
    std::uint64_t result = sizeof(VectorLayerData);
    for (const VectorFillBatch &batch : fillBatches_) {
        result += sizeof(VectorFillBatch);
        result += static_cast<std::uint64_t>(batch.vertices.size()) *
                  sizeof(VectorVertex2f);
        result += static_cast<std::uint64_t>(batch.indices.size()) *
                  sizeof(std::uint16_t);
    }
    result +=
        static_cast<std::uint64_t>(segments_.size()) * sizeof(VectorSegment2f);
    result +=
        static_cast<std::uint64_t>(markers_.size()) * sizeof(VectorVertex2f);
    return result;
}

void VectorGeometryBuilder::ensureRetained(const std::uint64_t addition) const
{
    ensureLimit("maximumRetainedBytes",
                retainedBytes(),
                addition,
                limits_.maximumRetainedBytes);
    ensureLimit("maximumApplicationWorkingBytes",
                retainedBytes(),
                addition,
                limits_.maximumApplicationWorkingBytes);
}

void VectorGeometryBuilder::ensureWorking(
    const std::uint64_t transientBytes) const
{
    ensureLimit("maximumApplicationWorkingBytes",
                retainedBytes(),
                transientBytes,
                limits_.maximumApplicationWorkingBytes);
}

void VectorGeometryBuilder::extendBounds(const double x, const double y)
{
    if (!hasBounds_) {
        minimum_ = {x, y};
        maximum_ = {x, y};
        hasBounds_ = true;
        return;
    }
    minimum_[0] = std::min(minimum_[0], x);
    minimum_[1] = std::min(minimum_[1], y);
    maximum_[0] = std::max(maximum_[0], x);
    maximum_[1] = std::max(maximum_[1], y);
}

void VectorGeometryBuilder::appendMarker(const VectorVertex2f &point)
{
    ensureLimit("maximumMarkers", markers_.size(), 1, limits_.maximumMarkers);
    ensureRetained(sizeof(VectorVertex2f));
    markers_.push_back(point);
}

void VectorGeometryBuilder::appendSegment(const VectorVertex2f &from,
                                          const VectorVertex2f &to)
{
    if (from == to) {
        return;
    }
    ensureLimit(
        "maximumSegments", segments_.size(), 1, limits_.maximumSegments);
    ensureRetained(sizeof(VectorSegment2f));
    segments_.push_back({.x0 = from.x, .y0 = from.y, .x1 = to.x, .y1 = to.y});
}

bool VectorGeometryBuilder::addPoint(const double x, const double y)
{
    if (!validCoordinate(x, y)) {
        ++summary_.skippedParts;
        return false;
    }
    appendMarker({.x = static_cast<float>(x - origin_.x),
                  .y = static_cast<float>(y - origin_.y)});
    extendBounds(x, y);
    ++summary_.pointParts;
    return true;
}

void VectorGeometryBuilder::markSkippedFeature() noexcept
{
    ++summary_.skippedFeatures;
}

void VectorGeometryBuilder::markSkippedPart() noexcept
{
    ++summary_.skippedParts;
}

void VectorGeometryBuilder::markSourceHadZ() noexcept
{
    summary_.sourceHadZ = true;
}

bool VectorGeometryBuilder::addLineString(
    const std::span<const std::array<double, 2>> vertices, const bool closed)
{
    ensureWorking(checkedMultiplyOrThrow(vertices.size(),
                                         sizeof(std::array<double, 2>),
                                         "maximumApplicationWorkingBytes"));
    std::vector<VectorVertex2f> cleaned;
    cleaned.reserve(vertices.size());
    for (const auto &vertex : vertices) {
        if (!validCoordinate(vertex[0], vertex[1])) {
            ++summary_.skippedParts;
            return false;
        }
        const VectorVertex2f relative{
            .x = static_cast<float>(vertex[0] - origin_.x),
            .y = static_cast<float>(vertex[1] - origin_.y)};
        if (cleaned.empty() || cleaned.back() != relative) {
            cleaned.push_back(relative);
        }
    }
    if (closed && cleaned.size() > 1 && cleaned.front() == cleaned.back()) {
        cleaned.pop_back();
    }
    if (cleaned.size() < 2) {
        ++summary_.skippedParts;
        return false;
    }
    for (std::size_t index = 1; index < cleaned.size(); ++index) {
        appendSegment(cleaned[index - 1], cleaned[index]);
    }
    if (closed) {
        appendSegment(cleaned.back(), cleaned.front());
    }
    for (const VectorVertex2f vertex : cleaned) {
        extendBounds(origin_.x + vertex.x, origin_.y + vertex.y);
    }
    ++summary_.lineParts;
    return true;
}

void VectorGeometryBuilder::appendFillTriangle(
    const std::vector<VectorVertex2f> &vertices,
    const std::array<std::uint32_t, 3> triangle,
    std::vector<RemapEntry> &remap,
    std::uint32_t &generation)
{
    std::uint64_t missing = 0;
    for (std::size_t index = 0; index < triangle.size(); ++index) {
        const std::uint32_t original = triangle[index];
        const bool earlier = std::ranges::any_of(
            triangle.begin(),
            triangle.begin() + static_cast<std::ptrdiff_t>(index),
            [original](const std::uint32_t candidate) {
                return candidate == original;
            });
        if (!earlier && remap[original].generation != generation) {
            ++missing;
        }
    }
    if (fillBatches_.empty()) {
        ensureRetained(sizeof(VectorFillBatch));
        fillBatches_.emplace_back();
    }
    if (fillBatches_.back().vertices.size() + missing >
        maximumVerticesPerFillBatch) {
        ++generation;
        if (generation == 0) {
            for (RemapEntry &entry : remap) {
                entry.generation = 0;
            }
            generation = 1;
        }
        ensureRetained(sizeof(VectorFillBatch));
        fillBatches_.emplace_back();
        missing = 0;
        for (std::size_t index = 0; index < triangle.size(); ++index) {
            const std::uint32_t original = triangle[index];
            const bool earlier = std::ranges::any_of(
                triangle.begin(),
                triangle.begin() + static_cast<std::ptrdiff_t>(index),
                [original](const std::uint32_t candidate) {
                    return candidate == original;
                });
            if (!earlier && remap[original].generation != generation) {
                ++missing;
            }
        }
    }
    ensureLimit("maximumEmittedFillVertices",
                emittedFillVertices_,
                missing,
                limits_.maximumEmittedFillVertices);
    ensureLimit(
        "maximumFillIndices", fillIndices_, 3, limits_.maximumFillIndices);
    ensureRetained(missing * sizeof(VectorVertex2f) +
                   3U * sizeof(std::uint16_t));
    VectorFillBatch &batch = fillBatches_.back();
    for (const std::uint32_t original : triangle) {
        RemapEntry &entry = remap[original];
        if (entry.generation != generation) {
            entry.generation = generation;
            entry.localIndex =
                static_cast<std::uint16_t>(batch.vertices.size());
            batch.vertices.push_back(vertices[original]);
            ++emittedFillVertices_;
        }
        batch.indices.push_back(entry.localIndex);
        ++fillIndices_;
    }
}

bool VectorGeometryBuilder::addPolygon(const std::span<const VectorRing> rings)
{
    std::uint64_t sourceVertices = 0;
    for (const VectorRing &ring : rings) {
        sourceVertices = checkedAddOrThrow(
            sourceVertices, ring.size(), "maximumInputPolygonVertices");
    }
    ensureLimit("maximumInputPolygonVertices",
                inputPolygonVertices_,
                sourceVertices,
                limits_.maximumInputPolygonVertices);
    inputPolygonVertices_ += sourceVertices;
    ensureWorking(checkedMultiplyOrThrow(sourceVertices,
                                         sizeof(std::array<double, 2>),
                                         "maximumApplicationWorkingBytes"));
    if (rings.empty()) {
        ++summary_.skippedParts;
        return false;
    }

    std::vector<VectorRing> normalized;
    normalized.reserve(rings.size());
    std::vector<VectorVertex2f> flat;
    for (const VectorRing &ring : rings) {
        VectorRing cleaned;
        cleaned.reserve(ring.size());
        for (const auto &vertex : ring) {
            if (!finite(vertex) || !validCoordinate(vertex[0], vertex[1])) {
                ++summary_.skippedParts;
                return false;
            }
            const std::array<double, 2> relative{vertex[0] - origin_.x,
                                                 vertex[1] - origin_.y};
            if (cleaned.empty() || !same(cleaned.back(), relative)) {
                cleaned.push_back(relative);
            }
        }
        if (cleaned.size() > 1 && same(cleaned.front(), cleaned.back())) {
            cleaned.pop_back();
        }
        if (cleaned.size() < 3) {
            ++summary_.skippedParts;
            return false;
        }
        normalized.push_back(std::move(cleaned));
    }
    for (const VectorRing &ring : normalized) {
        for (const auto &vertex : ring) {
            flat.push_back({.x = static_cast<float>(vertex[0]),
                            .y = static_cast<float>(vertex[1])});
        }
    }

    // Outlines are valuable even when the triangulation fails validation.
    for (const VectorRing &ring : normalized) {
        std::vector<VectorVertex2f> outline;
        outline.reserve(ring.size());
        for (const auto &vertex : ring) {
            outline.push_back({.x = static_cast<float>(vertex[0]),
                               .y = static_cast<float>(vertex[1])});
        }
        for (std::size_t index = 1; index < outline.size(); ++index) {
            appendSegment(outline[index - 1], outline[index]);
        }
        appendSegment(outline.back(), outline.front());
    }

    const std::vector<std::uint32_t> indices =
        mapbox::earcut<std::uint32_t>(normalized);
    bool valid = !indices.empty() && indices.size() % 3U == 0U;
    double expectedArea = std::abs(signedArea(normalized.front()));
    for (std::size_t index = 1; index < normalized.size(); ++index) {
        expectedArea -= std::abs(signedArea(normalized[index]));
    }
    double actualArea = 0.0;
    for (std::size_t index = 0; valid && index < indices.size(); index += 3U) {
        if (indices[index] >= flat.size() ||
            indices[index + 1U] >= flat.size() ||
            indices[index + 2U] >= flat.size()) {
            valid = false;
            break;
        }
        const double area = triangleArea(flat[indices[index]],
                                         flat[indices[index + 1U]],
                                         flat[indices[index + 2U]]);
        if (!std::isfinite(area)) {
            valid = false;
            break;
        }
        actualArea += area;
    }
    const double tolerance = std::max(1.0e-6, std::abs(expectedArea) * 0.01);
    valid = valid && std::isfinite(expectedArea) &&
            std::abs(actualArea - expectedArea) <= tolerance;
    if (!valid) {
        ++summary_.unfilledPolygons;
    } else {
        ensureWorking(checkedMultiplyOrThrow(
            flat.size(), sizeof(RemapEntry), "maximumApplicationWorkingBytes"));
        std::vector<RemapEntry> remap(flat.size());
        std::uint32_t generation = 1;
        for (std::size_t index = 0; index < indices.size(); index += 3U) {
            appendFillTriangle(
                flat,
                {indices[index], indices[index + 1U], indices[index + 2U]},
                remap,
                generation);
        }
    }
    for (const VectorRing &ring : normalized) {
        for (const auto &vertex : ring) {
            extendBounds(origin_.x + vertex[0], origin_.y + vertex[1]);
        }
    }
    ++summary_.polygonParts;
    return true;
}

VectorLayerData VectorGeometryBuilder::build() &&
{
    VectorLayerData result;
    result.origin = origin_;
    result.fillBatches = std::move(fillBatches_);
    result.segments = std::move(segments_);
    result.markers = std::move(markers_);
    result.summary = summary_;
    if (hasBounds_) {
        result.bounds = {
            .minimum = {minimum_[0], minimum_[1], 0.0},
            .maximum = {maximum_[0], maximum_[1], 0.0},
        };
    }
    return result;
}

} // namespace pci
