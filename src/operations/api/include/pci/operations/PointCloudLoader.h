#pragma once

#include <pci/operations/PointCloudImport.h>
#include <pci/operations/PreparedPointDataset.h>
#include <pci/runtime/HierarchyResidencyCoordinator.h>

#include <functional>
#include <stop_token>

namespace pci {

struct PointCloudLoadResources {
    std::uint64_t decodedByteBudget = defaultPointCloudDecodedByteBudget;
    HierarchyResidencyCoordinatorPtr residency;
    PointMemoryBudgetPtr memoryBudget;
    PointMemoryBudget::ReservationPtr flatReservation;
};

struct PointCloudLoadContext {
    std::stop_token stopToken;
    std::function<void(PointCloudImportProgress)> progress = {};
    std::function<void(PointDatasetEvent)> dataReady = {};
    PointImportToken token = {};
};

struct PointCloudLoadRequest {
    PointCloudLoadOptions options;
    PointCloudLoadResources resources;
    SessionGeneration session = {};
};

class PointCloudLoader {
public:
    virtual ~PointCloudLoader() = default;

    [[nodiscard]] virtual PointCloudImportPreflight
    inspect(const PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token stopToken) const
    {
        if (stopToken.stop_requested()) {
            throw PointCloudImportCancelled();
        }
        PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = options.maximumPoints;
        return {.metadata = std::move(metadata),
                .desiredRetainedPoints = options.maximumPoints,
                .estimatedResidentBytes =
                    estimatedFlatResidentBytes(options.maximumPoints),
                .estimatedActiveBytes = flatImportWorkingBytes,
                .hierarchical = false};
    }

    [[nodiscard]] virtual PreparedPointDatasetPtr
    load(const PointCloudLoadOptions &options,
         const PointCloudLoadResources &resources,
         const PointCloudImportPreflight &preflight,
         const PointCloudLoadContext &context) const = 0;
};

// Worker-local assembly. publish() copies a frozen seed and subsequent blocks
// are delivered separately. No mutable builder is shared with the consumer.
class PointDatasetPreparation final {
public:
    explicit PointDatasetPreparation(PointCloudMetadata metadata);
    PointDatasetPreparation(PointCloudMetadata metadata,
                            PointCloudDataSourcePtr source,
                            PointCloudNodePayloadPtr root,
                            bool initiallyComplete = false);
    void
    setResidentMemoryReservation(PointMemoryBudget::ReservationPtr reservation);
    void reserveRoot(const PointMemoryBudgetPtr &budget,
                     std::uint64_t allowance);
    void addBlock(PointBlockPtr block);
    void publish(const PointCloudLoadContext &context);
    [[nodiscard]] PreparedPointDatasetPtr finish();
    [[nodiscard]] std::uint64_t decodedResidentBytes() const noexcept;

private:
    std::shared_ptr<PreparedPointDataset> dataset_;
    PointCloudLoadContext context_;
    std::uint64_t sequence_ = 0;
    bool published_ = false;
    std::uint64_t payloadBytes_ = 0;
};

} // namespace pci
