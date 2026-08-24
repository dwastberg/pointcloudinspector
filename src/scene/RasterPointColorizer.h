#pragma once

#include "scene/RasterPointColorize.h"

#include <functional>
#include <optional>
#include <stop_token>

namespace pci {

[[nodiscard]] std::optional<RasterColorizePreparedPtr>
colorizePointCloudFromRaster(
    RasterColorizePreflight preflight,
    RasterTileSourcePtr raster,
    std::shared_ptr<const RasterDecodeParameters> decode,
    std::uint64_t rasterRenderGeneration,
    RasterColorizeOptions options,
    PointMemoryBudget::ReservationPtr colorReservation,
    PointMemoryBudget::ReservationPtr workingReservation,
    PointMemoryBudget::ReservationPtr rootStagingReservation,
    PointMemoryBudget::ReservationPtr flatStagingReservation,
    std::stop_token stop,
    std::function<void(RasterColorizeProgress)> progress = {});

} // namespace pci
