#include "scene/RasterColorizeRunStore.h"

#include "scene/RasterPointColorize.h"
#include "storage/SecureStorage.h"

#include "platform/QtPath.h"

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

void checkStop(const std::stop_token &stop)
{
    if (stop.stop_requested()) {
        throw PointCloudDataSourceCancelled();
    }
}

[[nodiscard]] std::unique_ptr<PrivateTemporaryDirectory>
createPrivateRunDirectory(const std::filesystem::path &baseDirectory)
{
    try {
        return std::make_unique<PrivateTemporaryDirectory>(
            baseDirectory, "pcinspector-colorize");
    } catch (const PrivateStorageError &error) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::Io,
            "Could not create a private colorization workspace: " +
                std::string(error.what()));
    }
}

} // namespace

RasterColorizeRunStore::RasterColorizeRunStore(std::filesystem::path directory)
    : privateDirectory_(createPrivateRunDirectory(directory))
    , directory_(privateDirectory_->path())
{
}

RasterColorizeRunStore::~RasterColorizeRunStore() = default;

std::array<std::byte, 12>
RasterColorizeRunStore::serialize(const RasterSortRecord &record) noexcept
{
    std::array<std::byte, 12> result{};
    for (std::uint32_t index = 0; index < 8; ++index) {
        result[index] = static_cast<std::byte>(record.address >> (index * 8U));
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        result[8U + index] =
            static_cast<std::byte>(record.destination >> (index * 8U));
    }
    return result;
}

RasterSortRecord RasterColorizeRunStore::deserialize(
    const std::span<const std::byte, 12> bytes) noexcept
{
    RasterSortRecord result;
    for (std::uint32_t index = 0; index < 8; ++index) {
        result.address |= static_cast<std::uint64_t>(
                              std::to_integer<std::uint8_t>(bytes[index]))
                          << (index * 8U);
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        result.destination |=
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[8U + index]))
            << (index * 8U);
    }
    return result;
}

void RasterColorizeRunStore::writeSortedRun(
    const std::span<const RasterSortRecord> records)
{
    if (records.empty()) {
        return;
    }
    if (!std::ranges::is_sorted(records)) {
        throw RasterColorizeError(RasterColorizeFailureCode::Internal,
                                  "Colorization run was not sorted");
    }
    const std::filesystem::path path =
        directory_ / ("run-" + std::to_string(runs_.size()) + ".bin");
    QFile output(pathToQString(path));
    if (!output.open(QIODeviceBase::WriteOnly | QIODeviceBase::NewOnly,
                     QFile::ReadOwner | QFile::WriteOwner)) {
        throw RasterColorizeError(RasterColorizeFailureCode::Io,
                                  "Could not create colorization run file");
    }
    try {
        for (const RasterSortRecord &record : records) {
            const auto bytes = serialize(record);
            if (output.write(reinterpret_cast<const char *>(bytes.data()),
                             static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size())) {
                throw RasterColorizeError(
                    RasterColorizeFailureCode::Io,
                    "Could not write colorization run file");
            }
        }
        if (!output.flush()) {
            throw RasterColorizeError(RasterColorizeFailureCode::Io,
                                      "Could not close colorization run file");
        }
        output.close();
    } catch (...) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    runs_.push_back({.path = path, .records = records.size()});
    records_ += records.size();
    bytes_ += records.size() * 12ULL;
    pathView_.clear();
}

void RasterColorizeRunStore::forEachMerged(
    const std::function<void(const RasterSortRecord &)> &visit,
    const std::stop_token stop) const
{
    forEachMergedAddressRange(0, std::nullopt, visit, stop);
}

void RasterColorizeRunStore::forEachMergedAddressRange(
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
            throw RasterColorizeError(RasterColorizeFailureCode::Io,
                                      "Could not read colorization run file");
        }
        return deserialize(bytes);
    };
    const auto readNext = [](Reader &reader) {
        std::array<std::byte, 12> bytes{};
        reader.input.read(reinterpret_cast<char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
        if (!reader.input) {
            throw RasterColorizeError(RasterColorizeFailureCode::Io,
                                      "Could not read colorization run file");
        }
        reader.current = deserialize(bytes);
        --reader.remaining;
    };

    for (const Run &run : runs_) {
        std::ifstream input(run.path, std::ios::binary);
        if (!input) {
            throw RasterColorizeError(RasterColorizeFailureCode::Io,
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
            throw RasterColorizeError(RasterColorizeFailureCode::Io,
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

std::size_t RasterColorizeRunStore::runCount() const noexcept
{
    return runs_.size();
}

std::uint64_t RasterColorizeRunStore::recordCount() const noexcept
{
    return records_;
}

std::uint64_t RasterColorizeRunStore::bytesWritten() const noexcept
{
    return bytes_;
}

const std::vector<std::filesystem::path> &
RasterColorizeRunStore::paths() const noexcept
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

} // namespace pci
