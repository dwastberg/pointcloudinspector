#pragma once

#include <pci/operations/RasterPointColorize.h>
#include <pci/runtime/point/PointColorInstallation.h>

#include <pci/operations/RasterColorizeRunStore.h>

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
    RasterColorizeRunStoreFactory runStoreFactory,
    std::stop_token stop,
    std::function<void(RasterColorizeProgress)> progress = {});

[[nodiscard]] PointColorInstallationPtr
preparePointColorInstallation(RasterColorizePreparedPtr prepared);

} // namespace pci
