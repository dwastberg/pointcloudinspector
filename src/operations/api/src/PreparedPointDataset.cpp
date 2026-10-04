#include <pci/operations/PreparedPointDataset.h>

#include <pci/foundation/CheckedArithmetic.h>

#include <stdexcept>
#include <utility>

namespace pci {
namespace {

std::uint64_t blockBytes(const PointBlock &block) noexcept
{
    return saturatingAdd<std::uint64_t>(
        saturatingMultiply<std::uint64_t>(block.points.capacity(),
                                          sizeof(GpuPoint)),
        saturatingMultiply<std::uint64_t>(block.attributes.capacity(),
                                          sizeof(PointAttributes)));
}

} // namespace

std::uint64_t PreparedPointDataset::pointCount() const noexcept
{
    if (source) {
        return descriptor.metadata.sourcePointCount;
    }
    std::uint64_t count = 0;
    for (const auto &block : blocks) {
        if (block) {
            count = saturatingAdd<std::uint64_t>(count, block->points.size());
        }
    }
    return count;
}

std::uint64_t PreparedPointDataset::payloadBytes() const noexcept
{
    std::uint64_t bytes = root ? pointCloudNodePayloadBytes(*root) : 0;
    for (const auto &block : blocks) {
        if (block) {
            bytes = saturatingAdd<std::uint64_t>(bytes, blockBytes(*block));
        }
    }
    return bytes;
}

std::uint64_t PointDatasetEvent::retainedBytes() const noexcept
{
    if (const auto *dataset = std::get_if<PreparedPointDatasetPtr>(&data)) {
        if (!*dataset) {
            return sizeof(PointDatasetEvent);
        }
        const auto &value = **dataset;
        const auto &metadata = value.descriptor.metadata;
        auto metadataBytes = saturatingMultiply<std::uint64_t>(
            metadata.sourcePath.native().capacity(),
            sizeof(std::filesystem::path::value_type));
        metadataBytes = saturatingAdd<std::uint64_t>(
            metadataBytes, metadata.sourceDriver.capacity());
        metadataBytes = saturatingAdd<std::uint64_t>(
            metadataBytes, metadata.spatialReferenceWkt.capacity());
        metadataBytes = saturatingAdd<std::uint64_t>(
            metadataBytes,
            saturatingMultiply<std::uint64_t>(metadata.dimensions.capacity(),
                                              sizeof(std::string)));
        for (const auto &dimension : metadata.dimensions) {
            metadataBytes = saturatingAdd<std::uint64_t>(metadataBytes,
                                                         dimension.capacity());
        }
        return saturatingAdd<std::uint64_t>(
            value.payloadBytes(),
            saturatingAdd<std::uint64_t>(
                sizeof(PointDatasetEvent) + sizeof(PreparedPointDataset),
                saturatingAdd<std::uint64_t>(metadataBytes,
                                             value.blocks.capacity() *
                                                 sizeof(PointBlockPtr))));
    }
    const auto &value = std::get<PreparedPointBlock>(data);
    return saturatingAdd<std::uint64_t>(
        sizeof(PointDatasetEvent), value.block ? blockBytes(*value.block) : 0);
}

PointBlockPtr
retainPointBlockReservation(PointBlockPtr block,
                            PointMemoryBudget::ReservationPtr reservation)
{
    return retainPointMemoryReservation(std::move(block),
                                        std::move(reservation));
}

} // namespace pci
