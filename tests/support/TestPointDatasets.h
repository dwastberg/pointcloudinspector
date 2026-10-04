#pragma once

#include <pci/pointcloud/PointDatasetDescriptor.h>

#include <atomic>
#include <cstdint>
#include <utility>

namespace pci::test {

[[nodiscard]] inline PointDatasetView
pointDataset(PointCloudMetadata metadata = {},
             PointDatasetAvailability availability = {})
{
    static std::atomic<std::uint64_t> nextSource{1};
    return {
        .descriptor =
            {
                .sourceId = PointCloudSourceId{nextSource.fetch_add(
                    1, std::memory_order_relaxed)},
                .metadata = std::move(metadata),
            },
        .availability = availability,
    };
}

} // namespace pci::test
