#pragma once

#include <pci/document/SceneLayer.h>

namespace pci {

// The complete source domain is stable as incremental point data becomes
// available. It is used for document fitting, visibility indexing, and color
// normalization that must not change as pages arrive.
[[nodiscard]] Bounds3d
pointSourceDomainBounds(const PointCloudLayerState &point);

// Available bounds describe only the point data that can currently be
// displayed. Flat progressive imports can therefore differ from their source
// domain until loading completes.
[[nodiscard]] Bounds3d pointAvailableBounds(const PointCloudLayerState &point);

[[nodiscard]] Bounds3d vectorDisplayBounds(const VectorLayerState &vector);
[[nodiscard]] Bounds3d rasterDisplayBounds(const RasterLayerState &raster);

} // namespace pci
