#pragma once

#include <pci/desktop/operations/PointCloudColorizeController.h>
#include <pci/desktop/operations/PointCloudLoadController.h>
#include <pci/desktop/operations/RasterElevationController.h>
#include <pci/desktop/operations/RasterLoadController.h>
#include <pci/desktop/operations/VectorLoadController.h>

#include <pci/operations/PointCloudStatistics.h>
#include <pci/operations/StorageMaintenance.h>

#include <memory>

namespace pci {

// The only production owner of import concurrency. Controllers borrow the same
// scheduler and cancel only their own jobs; the owner waits once at shutdown.
// Raster *tile streaming* deliberately does not use this scheduler: the raster
// controller borrows it for inspection and bounded range sampling only, so a
// long point-cloud import cannot starve interactive imagery.
struct ImportServices {
    std::shared_ptr<LoadJobIdSequence> jobIds =
        std::make_shared<LoadJobIdSequence>();
    std::unique_ptr<TaskScheduler> scheduler;
    std::unique_ptr<PointCloudLoadController> pointCloud;
    std::unique_ptr<VectorLoadController> vector;
    std::unique_ptr<RasterLoadController> raster;
    std::unique_ptr<RasterElevationController> rasterElevation;
    std::unique_ptr<PointCloudColorizeController> colorize;
    std::shared_ptr<const PointCloudStatisticsProvider> statistics;
    std::shared_ptr<const StorageMaintenance> storageMaintenance;
    std::filesystem::path workingDirectory;
    std::shared_ptr<const SpatialReferenceComparator> spatialReferences;

    [[nodiscard]] bool valid() const noexcept
    {
        return scheduler && pointCloud && vector && raster && rasterElevation &&
               colorize && statistics &&
               pointCloud->schedulerIdentity() == scheduler.get() &&
               vector->schedulerIdentity() == scheduler.get() &&
               raster->schedulerIdentity() == scheduler.get() &&
               rasterElevation->schedulerIdentity() == scheduler.get() &&
               colorize->schedulerIdentity() == scheduler.get();
    }
    void shutdown()
    {
        colorize.reset();
        pointCloud.reset();
        vector.reset();
        raster.reset();
        rasterElevation.reset();
        statistics.reset();
        storageMaintenance.reset();
        if (scheduler)
            scheduler->waitForIdle();
        scheduler.reset();
    }
};

} // namespace pci
