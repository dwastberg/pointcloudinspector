#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <pci/foundation/CheckedArithmetic.h>

#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/storage/SecureStorage.h>

#include <QFile>
#include <QIODeviceBase>

#include <algorithm>
#include <array>
#include <fstream>
#include <queue>
#include <string>
#include <system_error>

namespace pci {
namespace {

class QFileRasterColorizeRunFile final : public LocalRasterColorizeRunFile {
public:
    explicit QFileRasterColorizeRunFile(const std::filesystem::path &path)
        : output_(pathToQString(path))
    {
        if (!output_.open(QIODeviceBase::WriteOnly | QIODeviceBase::NewOnly,
                          QFile::ReadOwner | QFile::WriteOwner)) {
            throw RasterColorizeRunStoreError(
                "Could not create colorization run file");
        }
    }

    [[nodiscard]] std::int64_t
    write(const std::span<const std::byte> bytes) override
    {
        return output_.write(reinterpret_cast<const char *>(bytes.data()),
                             static_cast<qint64>(bytes.size()));
    }

    [[nodiscard]] bool flush() override
    {
        return output_.flush();
    }

    void close() noexcept override
    {
        output_.close();
    }

private:
    QFile output_;
};

[[nodiscard]] LocalRasterColorizeRunFileFactory defaultFileFactory()
{
    return [](const std::filesystem::path &path) {
        return std::make_unique<QFileRasterColorizeRunFile>(path);
    };
}

void checkStop(const std::stop_token &stop)
{
    if (stop.stop_requested()) {
        throw RasterColorizeRunStoreCancelled();
    }
}

[[nodiscard]] std::unique_ptr<PrivateTemporaryDirectory>
createPrivateRunDirectory(const std::filesystem::path &baseDirectory)
{
    try {
        return std::make_unique<PrivateTemporaryDirectory>(
            baseDirectory, "pcinspector-colorize");
    } catch (const PrivateStorageError &error) {
        throw RasterColorizeRunStoreError(
            "Could not create a private colorization workspace: " +
            std::string(error.what()));
    }
}

} // namespace

LocalRasterColorizeRunStore::LocalRasterColorizeRunStore(
    std::filesystem::path directory,
    LocalRasterColorizeRunFileFactory fileFactory)
    : privateDirectory_(createPrivateRunDirectory(directory))
    , directory_(privateDirectory_->path())
    , fileFactory_(fileFactory ? std::move(fileFactory) : defaultFileFactory())
{
}

LocalRasterColorizeRunStore::~LocalRasterColorizeRunStore() = default;

void LocalRasterColorizeRunStore::prepareDescriptorCapacity()
{
    if (runs_.size() < runs_.capacity())
        return;
    if (runs_.size() == runs_.max_size())
        throw RasterColorizeRunStoreError(
            "Colorization run descriptor limit reached");
    if (beforeDescriptorAllocation_)
        beforeDescriptorAllocation_();
    const auto capacity = runs_.capacity();
    runs_.reserve(capacity == 0                      ? 1
                  : capacity <= runs_.max_size() / 2 ? capacity * 2
                                                     : runs_.max_size());
}

void LocalRasterColorizeRunStore::writeSortedRun(
    const std::span<const RasterSortRecord> records,
    const std::span<std::byte> scratch,
    const std::stop_token stop)
{
    if (records.empty()) {
        return;
    }
    checkStop(stop);
    const auto recordsPerChunk =
        std::min<std::size_t>(scratch.size(), 64 * 1024) / 12;
    if (recordsPerChunk == 0)
        throw RasterColorizeRunStoreError(
            "Colorization writer requires admitted scratch for one record");
    if (!std::ranges::is_sorted(records)) {
        throw RasterColorizeRunStoreError("Colorization run was not sorted");
    }
    const auto runBytes = checkedMultiply<std::uint64_t>(records.size(), 12);
    const auto nextRecords =
        checkedAdd<std::uint64_t>(records_, records.size());
    const auto nextBytes =
        runBytes ? checkedAdd(bytes_, *runBytes) : std::nullopt;
    if (!nextRecords || !nextBytes)
        throw RasterColorizeRunStoreError("Colorization run totals overflow");
    prepareDescriptorCapacity();
    const std::filesystem::path path =
        directory_ / ("run-" + std::to_string(runs_.size()) + ".bin");
    Run descriptor{.path = path, .records = records.size()};
    std::unique_ptr<LocalRasterColorizeRunFile> output = fileFactory_(path);
    if (!output) {
        throw RasterColorizeRunStoreError(
            "Could not create colorization run file");
    }
    try {
        for (std::size_t first = 0; first < records.size();) {
            checkStop(stop);
            const auto count =
                std::min(recordsPerChunk, records.size() - first);
            for (std::size_t i = 0; i < count; ++i) {
                const auto bytes =
                    serializeRasterSortRecord(records[first + i]);
                std::ranges::copy(bytes,
                                  scratch.begin() +
                                      static_cast<std::ptrdiff_t>(i * 12));
            }
            auto remaining = scratch.first(count * 12);
            while (!remaining.empty()) {
                checkStop(stop);
                const auto written = output->write(remaining);
                if (written <= 0 ||
                    static_cast<std::uint64_t>(written) > remaining.size())
                    throw RasterColorizeRunStoreError(
                        "Could not write colorization run file");
                remaining =
                    remaining.subspan(static_cast<std::size_t>(written));
            }
            first += count;
        }
        if (!output->flush()) {
            throw RasterColorizeRunStoreError(
                "Could not close colorization run file");
        }
        output->close();
        checkStop(stop);
        static_assert(std::is_nothrow_move_constructible_v<Run>);
        runs_.push_back(std::move(descriptor));
        records_ = *nextRecords;
        bytes_ = *nextBytes;
        pathView_.clear();
    } catch (...) {
        output->close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}

void LocalRasterColorizeRunStore::forEachMerged(
    const std::function<void(const RasterSortRecord &)> &visit,
    const std::stop_token stop) const
{
    forEachMergedAddressRange(0, std::nullopt, visit, stop);
}

void LocalRasterColorizeRunStore::forEachMergedAddressRange(
    const std::uint64_t lowerAddress,
    const std::optional<std::uint64_t> upperAddress,
    const std::function<void(const RasterSortRecord &)> &visit,
    const std::stop_token stop) const
{
    struct Reader {
        std::ifstream input;
        std::uint64_t remaining = 0;
        RasterSortRecord current;
    };
    struct QueueEntry {
        RasterSortRecord record;
        std::size_t reader = 0;
    };
    const auto later = [](const QueueEntry &left, const QueueEntry &right) {
        return left.record > right.record;
    };

    std::vector<Reader> readers;
    readers.reserve(runs_.size());
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, decltype(later)>
        queue(later);

    const auto readRecord = [](std::ifstream &input,
                               const std::uint64_t index) {
        std::array<std::byte, 12> bytes{};
        input.clear();
        input.seekg(static_cast<std::streamoff>(index * bytes.size()));
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            throw RasterColorizeRunStoreError(
                "Could not read colorization run file");
        }
        return deserializeRasterSortRecord(bytes);
    };
    const auto readNext = [](Reader &reader) {
        std::array<std::byte, 12> bytes{};
        reader.input.read(reinterpret_cast<char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
        if (!reader.input) {
            throw RasterColorizeRunStoreError(
                "Could not read colorization run file");
        }
        reader.current = deserializeRasterSortRecord(bytes);
        --reader.remaining;
    };

    for (const Run &run : runs_) {
        std::ifstream input(run.path, std::ios::binary);
        if (!input) {
            throw RasterColorizeRunStoreError(
                "Could not reopen colorization run file");
        }
        const auto lowerBound = [&](const std::uint64_t address) {
            std::uint64_t first = 0;
            std::uint64_t count = run.records;
            while (count > 0) {
                checkStop(stop);
                const std::uint64_t step = count / 2;
                const std::uint64_t middle = first + step;
                if (readRecord(input, middle).address < address) {
                    first = middle + 1;
                    count -= step + 1;
                } else {
                    count = step;
                }
            }
            return first;
        };
        const std::uint64_t first = lowerBound(lowerAddress);
        const std::uint64_t last =
            upperAddress ? lowerBound(*upperAddress) : run.records;
        input.clear();
        input.seekg(static_cast<std::streamoff>(first * 12ULL));
        if (!input) {
            throw RasterColorizeRunStoreError(
                "Could not seek colorization run file");
        }
        Reader reader{.input = std::move(input),
                      .remaining = last - first,
                      .current = {}};
        const std::size_t index = readers.size();
        if (reader.remaining > 0) {
            readNext(reader);
            queue.push({.record = reader.current, .reader = index});
        }
        readers.push_back(std::move(reader));
    }

    std::uint64_t visited = 0;
    while (!queue.empty()) {
        if ((visited++ & 4095U) == 0) {
            checkStop(stop);
        }
        const QueueEntry entry = queue.top();
        queue.pop();
        visit(entry.record);
        Reader &reader = readers[entry.reader];
        if (reader.remaining > 0) {
            readNext(reader);
            queue.push({.record = reader.current, .reader = entry.reader});
        }
    }
}

std::size_t LocalRasterColorizeRunStore::runCount() const noexcept
{
    return runs_.size();
}

std::uint64_t LocalRasterColorizeRunStore::recordCount() const noexcept
{
    return records_;
}

std::uint64_t LocalRasterColorizeRunStore::bytesWritten() const noexcept
{
    return bytes_;
}

const std::vector<std::filesystem::path> &
LocalRasterColorizeRunStore::paths() const noexcept
{
    if (pathView_.size() != runs_.size()) {
        pathView_.clear();
        pathView_.reserve(runs_.size());
        for (const Run &run : runs_) {
            pathView_.push_back(run.path);
        }
    }
    return pathView_;
}

RasterColorizeRunStoreFactory makeLocalRasterColorizeRunStoreFactory()
{
    return [](const std::filesystem::path &directory) {
        return std::make_unique<LocalRasterColorizeRunStore>(directory);
    };
}

} // namespace pci
