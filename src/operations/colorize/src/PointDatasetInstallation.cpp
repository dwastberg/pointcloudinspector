#include <pci/operations/PointDatasetInstallation.h>

#include <stdexcept>

namespace pci {

PointDatasetRuntimePtr
createPointDatasetRuntime(const PreparedPointDatasetPtr &dataset,
                          const std::uint64_t decodedByteBudget)
{
    if (!dataset || !dataset->descriptor.sourceId.value() ||
        bool(dataset->source) != bool(dataset->root) ||
        (dataset->source && !dataset->blocks.empty())) {
        throw std::invalid_argument("invalid prepared point dataset");
    }
    PointDatasetRuntimePtr runtime;
    if (dataset->source) {
        runtime =
            std::make_shared<PointDatasetRuntime>(dataset->descriptor.metadata,
                                                  dataset->source,
                                                  dataset->root,
                                                  decodedByteBudget,
                                                  dataset->loadingComplete,
                                                  dataset->descriptor.sourceId,
                                                  dataset->reservation);
    } else {
        runtime = std::make_shared<PointDatasetRuntime>(
            dataset->descriptor.metadata, dataset->descriptor.sourceId);
        runtime->setResidentMemoryReservation(dataset->reservation);
        for (const auto &block : dataset->blocks) {
            runtime->addBlock(block);
        }
        if (dataset->loadingComplete) {
            runtime->markLoadingComplete();
        }
    }
    return runtime;
}

void validatePointDatasetCompletion(const PointDatasetRuntimePtr &runtime,
                                    const PreparedPointDatasetPtr &dataset)
{
    if (!runtime || !dataset ||
        runtime->sourceId() != dataset->descriptor.sourceId ||
        runtime->hierarchical() != bool(dataset->source) ||
        !dataset->loadingComplete) {
        throw std::invalid_argument(
            "point completion does not match its preview");
    }
    if (!dataset->source) {
        const auto blocks = runtime->blocks();
        if (blocks != dataset->blocks) {
            throw std::invalid_argument(
                "point completion does not match ingested blocks");
        }
    }
}

void completePointDatasetRuntime(const PointDatasetRuntimePtr &runtime,
                                 const PreparedPointDatasetPtr &dataset)
{
    validatePointDatasetCompletion(runtime, dataset);
    runtime->markLoadingComplete();
}

} // namespace pci
