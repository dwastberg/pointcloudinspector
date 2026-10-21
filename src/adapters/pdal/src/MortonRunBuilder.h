#pragma once

#include "MortonRunRecord.h"
#include <pci/adapters/pdal/LocalPointPageFormat.h>
#include <pci/pointcloud/BlockPartitioner.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace pci::local_index {

class MortonRunBuilder final {
public:
    MortonRunBuilder(std::filesystem::path runDirectory,
                     std::uint64_t memoryBytes,
                     Bounds3d bounds,
                     std::uint8_t mortonLevel);
    ~MortonRunBuilder();

    MortonRunBuilder(MortonRunBuilder &&) noexcept;
    MortonRunBuilder &operator=(MortonRunBuilder &&) noexcept;
    MortonRunBuilder(const MortonRunBuilder &) = delete;
    MortonRunBuilder &operator=(const MortonRunBuilder &) = delete;

    void add(const PointSample &sample, std::uint64_t ordinal);
    void finish();
    [[nodiscard]] const std::vector<std::filesystem::path> &
    runs() const noexcept;
    [[nodiscard]] std::span<const MortonRunRecord>
    inMemoryRecords() const noexcept;
    [[nodiscard]] static std::size_t recordBytes() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pci::local_index
