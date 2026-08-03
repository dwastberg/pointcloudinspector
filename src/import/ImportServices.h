#pragma once

#include "import/PointCloudLoadController.h"
#include "import/PointCloudStatistics.h"
#include "import/VectorLoadController.h"

#include <memory>

namespace pci {

// The only production owner of import concurrency. Controllers borrow the same
// scheduler and cancel only their own jobs; the owner waits once at shutdown.
struct ImportServices {
    std::unique_ptr<TaskScheduler> scheduler;
    std::unique_ptr<PointCloudLoadController> pointCloud;
    std::unique_ptr<VectorLoadController> vector;
    std::shared_ptr<const PointCloudStatisticsProvider> statistics;

    [[nodiscard]] bool valid() const noexcept
    {
        return scheduler && pointCloud && vector && statistics &&
               pointCloud->schedulerIdentity() == scheduler.get() &&
               vector->schedulerIdentity() == scheduler.get();
    }
    void shutdown()
    {
        pointCloud.reset();
        vector.reset();
        statistics.reset();
        if (scheduler)
            scheduler->waitForIdle();
        scheduler.reset();
    }
};

} // namespace pci
