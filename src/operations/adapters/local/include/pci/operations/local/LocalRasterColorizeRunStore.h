#pragma once

#include <pci/operations/RasterColorizeRunStore.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace pci {

class PrivateTemporaryDirectory;

class LocalRasterColorizeRunFile {
public:
    virtual ~LocalRasterColorizeRunFile() = default;

    [[nodiscard]] virtual std::int64_t
    write(std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual bool flush() = 0;
    virtual void close() noexcept = 0;
};

using LocalRasterColorizeRunFileFactory =
    std::function<std::unique_ptr<LocalRasterColorizeRunFile>(
        const std::filesystem::path &)>;

class LocalRasterColorizeRunStore final : public RasterColorizeRunStore {
public:
    explicit LocalRasterColorizeRunStore(
        std::filesystem::path directory,
        LocalRasterColorizeRunFileFactory fileFactory = {});
    ~LocalRasterColorizeRunStore() override;

    LocalRasterColorizeRunStore(const LocalRasterColorizeRunStore &) = delete;
    LocalRasterColorizeRunStore &
    operator=(const LocalRasterColorizeRunStore &) = delete;

    void writeSortedRun(std::span<const RasterSortRecord> records,
                        std::span<std::byte> scratch,
                        std::stop_token stop) override;
    void
    forEachMerged(const std::function<void(const RasterSortRecord &)> &visit,
                  std::stop_token stop) const override;
    void forEachMergedAddressRange(
        std::uint64_t lowerAddress,
        std::optional<std::uint64_t> upperAddress,
        const std::function<void(const RasterSortRecord &)> &visit,
        std::stop_token stop) const override;

    [[nodiscard]] std::size_t runCount() const noexcept override;
    [[nodiscard]] std::uint64_t recordCount() const noexcept override;
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept override;
    [[nodiscard]] const std::vector<std::filesystem::path> &
    paths() const noexcept;

private:
    friend struct LocalRasterColorizeRunStoreTestAccess;
    void prepareDescriptorCapacity();
    std::function<void()> beforeDescriptorAllocation_;
    struct Run {
        std::filesystem::path path;
        std::uint64_t records = 0;
    };

    std::unique_ptr<PrivateTemporaryDirectory> privateDirectory_;
    std::filesystem::path directory_;
    LocalRasterColorizeRunFileFactory fileFactory_;
    std::vector<Run> runs_;
    mutable std::vector<std::filesystem::path> pathView_;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
};

[[nodiscard]] RasterColorizeRunStoreFactory
makeLocalRasterColorizeRunStoreFactory();

} // namespace pci
