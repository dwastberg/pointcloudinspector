#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace pci {

struct RasterSortRecord {
    std::uint64_t address = 0;
    std::uint32_t destination = 0;

    auto operator<=>(const RasterSortRecord &) const = default;
};
static_assert(sizeof(RasterSortRecord) == 16);

[[nodiscard]] std::array<std::byte, 12>
serializeRasterSortRecord(const RasterSortRecord &record) noexcept;
[[nodiscard]] RasterSortRecord
deserializeRasterSortRecord(std::span<const std::byte, 12> bytes) noexcept;

class RasterColorizeRunStoreError : public std::runtime_error {
public:
    explicit RasterColorizeRunStoreError(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

class RasterColorizeRunStoreCancelled final
    : public RasterColorizeRunStoreError {
public:
    RasterColorizeRunStoreCancelled()
        : RasterColorizeRunStoreError("Raster colorization was cancelled")
    {
    }
};

class RasterColorizeRunStore {
public:
    virtual ~RasterColorizeRunStore() = default;

    virtual void writeSortedRun(std::span<const RasterSortRecord> records,
                                std::span<std::byte> scratch,
                                std::stop_token stop) = 0;
    virtual void
    forEachMerged(const std::function<void(const RasterSortRecord &)> &visit,
                  std::stop_token stop) const = 0;
    virtual void forEachMergedAddressRange(
        std::uint64_t lowerAddress,
        std::optional<std::uint64_t> upperAddress,
        const std::function<void(const RasterSortRecord &)> &visit,
        std::stop_token stop) const = 0;

    [[nodiscard]] virtual std::size_t runCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t recordCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t bytesWritten() const noexcept = 0;
};

using RasterColorizeRunStoreFactory =
    std::function<std::unique_ptr<RasterColorizeRunStore>(
        const std::filesystem::path &)>;

} // namespace pci
