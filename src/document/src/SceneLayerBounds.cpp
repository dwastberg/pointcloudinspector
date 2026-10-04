#include <pci/document/SceneLayerBounds.h>

namespace pci {

Bounds3d pointAvailableBounds(const PointCloudLayerState &point)
{
    return point.availableBounds;
}

Bounds3d pointSourceDomainBounds(const PointCloudLayerState &point)
{
    const Bounds3d source = point.descriptor.metadata.sourceBounds;
    return source.valid() ? source : pointAvailableBounds(point);
}

Bounds3d vectorDisplayBounds(const VectorLayerState &vector)
{
    if (!vector.data) {
        return {};
    }
    Bounds3d result = vector.data->bounds;
    result.minimum[2] += vector.style.zOffset;
    result.maximum[2] += vector.style.zOffset;
    for (std::size_t axis = 0; axis < result.minimum.size(); ++axis) {
        if (result.minimum[axis] == result.maximum[axis]) {
            result.minimum[axis] -= 0.5;
            result.maximum[axis] += 0.5;
        }
    }
    return result;
}

Bounds3d rasterDisplayBounds(const RasterLayerState &raster)
{
    return rasterSceneBounds(raster.descriptor.metadata,
                             raster.style,
                             raster.elevationStatus,
                             raster.exactElevationRange);
}

} // namespace pci
