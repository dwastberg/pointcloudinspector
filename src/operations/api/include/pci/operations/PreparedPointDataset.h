#pragma once

#include <pci/foundation/Generation.h>
#include <pci/pointcloud/PointCloudDataSource.h>
#include <pci/pointcloud/PointDatasetDescriptor.h>
#include <pci/runtime/PointMemoryBudget.h>

#include <memory>
#include <variant>
#include <vector>

namespace pci {

struct PointImportToken {
    SessionGeneration session;
    std::uint64_t attempt = 0;
    auto operator<=>(const PointImportToken &) const = default;
};

// This value owns data only. A published instance is immutable; it cannot
// decode, schedule work, or modify a document/runtime binding.
struct PreparedPointDataset {
    PointDatasetDescriptor descriptor;
    PointMemoryBudget::ReservationPtr reservation;
    std::vector<PointBlockPtr> blocks;
    PointCloudDataSourcePtr source;
    PointCloudNodePayloadPtr root;
    // Number of events published before this result (zero for standalone
    // loads).
    std::uint64_t eventCount = 0;
    bool loadingComplete = false;

    [[nodiscard]] std::uint64_t pointCount() const noexcept;
    [[nodiscard]] std::uint64_t payloadBytes() const noexcept;
};

using PreparedPointDatasetPtr = std::shared_ptr<const PreparedPointDataset>;

struct PreparedPointBlock {
    PointCloudSourceId sourceId;
    std::uint64_t index = 0;
    PointBlockPtr block;
};

struct PointDatasetEvent {
    PointImportToken token;
    std::uint64_t sequence = 0;
    std::variant<PreparedPointDatasetPtr, PreparedPointBlock> data;

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;
};

[[nodiscard]] PointBlockPtr
retainPointBlockReservation(PointBlockPtr block,
                            PointMemoryBudget::ReservationPtr reservation);

} // namespace pci
