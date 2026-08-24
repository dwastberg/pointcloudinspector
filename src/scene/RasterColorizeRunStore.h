#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace pci {

struct RasterSortRecord {
    std::uint64_t address = 0;
    std::uint32_t destination = 0;

    auto operator<=>(const RasterSortRecord &) const = default;
};
static_assert(sizeof(RasterSortRecord) == 16);

class RasterColorizeRunStore final {
public:
    explicit RasterColorizeRunStore(std::filesystem::path directory);
    ~RasterColorizeRunStore();

    RasterColorizeRunStore(const RasterColorizeRunStore &) = delete;
    RasterColorizeRunStore &operator=(const RasterColorizeRunStore &) = delete;

    void writeSortedRun(std::span<const RasterSortRecord> records);
    void
    forEachMerged(const std::function<void(const RasterSortRecord &)> &visit,
                  std::stop_token stop) const;
    void forEachMergedAddressRange(
        std::uint64_t lowerAddress,
        std::optional<std::uint64_t> upperAddress,
        const std::function<void(const RasterSortRecord &)> &visit,
        std::stop_token stop) const;

    [[nodiscard]] std::size_t runCount() const noexcept;
    [[nodiscard]] std::uint64_t recordCount() const noexcept;
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept;
    [[nodiscard]] const std::vector<std::filesystem::path> &
    paths() const noexcept;

    [[nodiscard]] static std::array<std::byte, 12>
    serialize(const RasterSortRecord &record) noexcept;
    [[nodiscard]] static RasterSortRecord
    deserialize(std::span<const std::byte, 12> bytes) noexcept;

private:
    struct Run {
        std::filesystem::path path;
        std::uint64_t records = 0;
    };

    std::filesystem::path directory_;
    std::vector<Run> runs_;
    mutable std::vector<std::filesystem::path> pathView_;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
};

} // namespace pci
