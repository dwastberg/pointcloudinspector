#pragma once

#include <pci/operations/RasterColorizeRunStore.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pci::test {

class InMemoryRasterColorizeRunStore final : public RasterColorizeRunStore {
public:
    explicit InMemoryRasterColorizeRunStore(
        std::function<void(std::span<std::byte>)> observer = {})
        : observer_(std::move(observer))
    {
    }
    void writeSortedRun(const std::span<const RasterSortRecord> records,
                        const std::span<std::byte> scratch,
                        std::stop_token stop) override
    {
        if (records.empty()) {
            return;
        }
        if (stop.stop_requested())
            throw RasterColorizeRunStoreCancelled();
        if (scratch.size() < 12)
            throw RasterColorizeRunStoreError("No admitted writer scratch");
        if (!std::ranges::is_sorted(records)) {
            throw std::invalid_argument("in-memory run must be sorted");
        }
        if (observer_)
            observer_(scratch);
        runs_.emplace_back(records.begin(), records.end());
        records_ += records.size();
    }

    void
    forEachMerged(const std::function<void(const RasterSortRecord &)> &visit,
                  const std::stop_token stop) const override
    {
        forEachMergedAddressRange(0, std::nullopt, visit, stop);
    }

    void forEachMergedAddressRange(
        const std::uint64_t lowerAddress,
        const std::optional<std::uint64_t> upperAddress,
        const std::function<void(const RasterSortRecord &)> &visit,
        const std::stop_token stop) const override
    {
        std::vector<RasterSortRecord> selected;
        for (const auto &run : runs_) {
            for (const RasterSortRecord &record : run) {
                if (record.address >= lowerAddress &&
                    (!upperAddress || record.address < *upperAddress)) {
                    selected.push_back(record);
                }
            }
        }
        std::ranges::sort(selected);
        for (const RasterSortRecord &record : selected) {
            if (stop.stop_requested()) {
                throw RasterColorizeRunStoreCancelled();
            }
            visit(record);
        }
    }

    [[nodiscard]] std::size_t runCount() const noexcept override
    {
        return runs_.size();
    }

    [[nodiscard]] std::uint64_t recordCount() const noexcept override
    {
        return records_;
    }

    [[nodiscard]] std::uint64_t bytesWritten() const noexcept override
    {
        return records_ * 12U;
    }

private:
    std::function<void(std::span<std::byte>)> observer_;
    std::vector<std::vector<RasterSortRecord>> runs_;
    std::uint64_t records_ = 0;
};

inline RasterColorizeRunStoreFactory inMemoryRasterColorizeRunStoreFactory()
{
    return [](const std::filesystem::path &) {
        return std::make_unique<InMemoryRasterColorizeRunStore>();
    };
}

} // namespace pci::test
