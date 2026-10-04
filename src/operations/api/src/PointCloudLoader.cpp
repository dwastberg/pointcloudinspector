#include <pci/operations/PointCloudLoader.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <stdexcept>
#include <utility>

namespace pci {

PointDatasetPreparation::PointDatasetPreparation(PointCloudMetadata metadata)
    : dataset_(std::make_shared<PreparedPointDataset>())
{
    dataset_->descriptor = {allocatePointCloudSourceId(), std::move(metadata)};
}

PointDatasetPreparation::PointDatasetPreparation(PointCloudMetadata metadata,
                                                 PointCloudDataSourcePtr source,
                                                 PointCloudNodePayloadPtr root,
                                                 const bool initiallyComplete)
    : PointDatasetPreparation(std::move(metadata))
{
    if (!source || !root || root->nodeId != rootPointCloudNode) {
        throw std::invalid_argument(
            "prepared hierarchy requires a source and root");
    }
    dataset_->source = std::move(source);
    payloadBytes_ = pointCloudNodePayloadBytes(*root);
    dataset_->root = std::move(root);
    dataset_->loadingComplete = initiallyComplete;
}

void PointDatasetPreparation::setResidentMemoryReservation(
    PointMemoryBudget::ReservationPtr reservation)
{
    if (published_ || !dataset_->blocks.empty() || dataset_->reservation) {
        throw std::logic_error("point reservation must precede publication");
    }
    dataset_->reservation = std::move(reservation);
    if (dataset_->root && dataset_->reservation) {
        auto root = std::make_shared<PointCloudNodePayload>(*dataset_->root);
        for (auto &block : root->blocks) {
            block = retainPointBlockReservation(std::move(block),
                                                dataset_->reservation);
        }
        dataset_->root = std::move(root);
    }
}

void PointDatasetPreparation::reserveRoot(const PointMemoryBudgetPtr &budget,
                                          const std::uint64_t allowance)
{
    if (!budget || !dataset_->root) {
        return;
    }
    const auto available = std::min(allowance, budget->availableBytes());
    if (payloadBytes_ > available) {
        dataset_->root = reducedRootPayload(*dataset_->root, available);
        payloadBytes_ = pointCloudNodePayloadBytes(*dataset_->root);
    }
    auto reservation = budget->tryReserve(payloadBytes_);
    if (!reservation) {
        throw PointCloudImportError(
            "hierarchy preview reservation was refused");
    }
    setResidentMemoryReservation(std::move(*reservation));
}

void PointDatasetPreparation::addBlock(PointBlockPtr block)
{
    if (!dataset_ || dataset_->source || !block || block->points.empty()) {
        throw std::invalid_argument("prepared flat block is invalid");
    }
    if (context_.stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }
    block =
        retainPointBlockReservation(std::move(block), dataset_->reservation);
    const std::uint64_t index = dataset_->blocks.size();
    const auto blockBytes = saturatingAdd<std::uint64_t>(
        saturatingMultiply<std::uint64_t>(block->points.capacity(),
                                          sizeof(GpuPoint)),
        saturatingMultiply<std::uint64_t>(block->attributes.capacity(),
                                          sizeof(PointAttributes)));
    payloadBytes_ = saturatingAdd(payloadBytes_, blockBytes);
    dataset_->blocks.push_back(block);
    if (dataset_->reservation &&
        payloadBytes_ > dataset_->reservation->bytes()) {
        throw PointCloudImportError(
            "flat payload exceeded its admitted reservation");
    }
    if (published_ && context_.dataReady) {
        context_.dataReady({context_.token,
                            ++sequence_,
                            PreparedPointBlock{dataset_->descriptor.sourceId,
                                               index,
                                               std::move(block)}});
    }
}

void PointDatasetPreparation::publish(const PointCloudLoadContext &context)
{
    if (published_ || !dataset_) {
        throw std::logic_error("point dataset seed was already published");
    }
    context_ = context;
    if (context_.stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }
    published_ = true;
    if (context_.dataReady) {
        context_.dataReady(
            {context_.token,
             0,
             std::make_shared<const PreparedPointDataset>(*dataset_)});
    }
}

PreparedPointDatasetPtr PointDatasetPreparation::finish()
{
    if (!dataset_) {
        throw std::logic_error("point dataset preparation already finished");
    }
    if (context_.stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }
    dataset_->loadingComplete = true;
    dataset_->eventCount = published_ && context_.dataReady ? sequence_ + 1 : 0;
    return std::exchange(dataset_, {});
}

std::uint64_t PointDatasetPreparation::decodedResidentBytes() const noexcept
{
    return payloadBytes_;
}

} // namespace pci
