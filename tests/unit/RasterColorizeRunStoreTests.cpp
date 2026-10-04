#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace pci {
struct LocalRasterColorizeRunStoreTestAccess {
    static void failPublication(LocalRasterColorizeRunStore &store)
    {
        store.beforeDescriptorAllocation_ = [] {
            throw std::bad_alloc();
        };
    }
    static const std::filesystem::path &
    directory(const LocalRasterColorizeRunStore &store)
    {
        return store.directory_;
    }
};
} // namespace pci

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(QDir(directory_.path()).filesystemPath())
    {
        if (!directory_.isValid()) {
            throw std::runtime_error(
                "could not create private run-store fixture");
        }
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

public:
    std::array<std::byte, 64 * 1024> scratch{};

private:
    QTemporaryDir directory_;
    std::filesystem::path path_;
};

struct RecordingFileState {
    std::vector<std::byte> bytes;
    std::size_t maximumWrite = std::numeric_limits<std::size_t>::max();
    std::size_t writes = 0;
    std::size_t flushes = 0;
    std::size_t closes = 0;
    bool flushFails = false;
    bool writeFails = false;
    std::size_t failWriteNumber = 0;
    std::stop_source stop;
    bool stopOnWrite = false;
    bool stopOnClose = false;
};

class RecordingRunFile final : public pci::LocalRasterColorizeRunFile {
public:
    explicit RecordingRunFile(std::shared_ptr<RecordingFileState> state)
        : state_(std::move(state))
    {
    }

    [[nodiscard]] std::int64_t
    write(const std::span<const std::byte> bytes) override
    {
        ++state_->writes;
        if (state_->writeFails || state_->writes == state_->failWriteNumber)
            return -1;
        if (state_->stopOnWrite)
            state_->stop.request_stop();
        const std::size_t accepted =
            std::min(bytes.size(), state_->maximumWrite);
        state_->bytes.insert(state_->bytes.end(),
                             bytes.begin(),
                             bytes.begin() +
                                 static_cast<std::ptrdiff_t>(accepted));
        return static_cast<std::int64_t>(accepted);
    }

    [[nodiscard]] bool flush() override
    {
        ++state_->flushes;
        return !state_->flushFails;
    }

    void close() noexcept override
    {
        ++state_->closes;
        if (state_->stopOnClose)
            state_->stop.request_stop();
    }

private:
    std::shared_ptr<RecordingFileState> state_;
};

[[nodiscard]] pci::LocalRasterColorizeRunFileFactory
recordingFileFactory(const std::shared_ptr<RecordingFileState> &state)
{
    return [state](const std::filesystem::path &path) {
        std::ofstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("fixture file creation failed");
        return std::make_unique<RecordingRunFile>(state);
    };
}

TEST_CASE("raster colorize run store merges runs and cleans files",
          "[unit][scene][colorize]")
{
    TemporaryDirectory directory;
    std::vector<std::filesystem::path> paths;
    {
        pci::LocalRasterColorizeRunStore store(directory.path());
        const std::array first{
            pci::RasterSortRecord{.address = 1, .destination = 4},
            pci::RasterSortRecord{.address = 5, .destination = 2},
            pci::RasterSortRecord{.address = 9, .destination = 0},
        };
        const std::array second{
            pci::RasterSortRecord{.address = 1, .destination = 7},
            pci::RasterSortRecord{.address = 4, .destination = 3},
            pci::RasterSortRecord{.address = 10, .destination = 1},
        };
        store.writeSortedRun(first, directory.scratch, {});
        store.writeSortedRun(second, directory.scratch, {});
        REQUIRE(store.runCount() == 2);
        CHECK(store.recordCount() == 6);
        CHECK(store.bytesWritten() == 72);
        paths = store.paths();
        REQUIRE(paths.size() == 2);
        CHECK(paths[0] != paths[1]);
        CHECK(std::filesystem::exists(paths[0]));
        CHECK(std::filesystem::exists(paths[1]));

        std::vector<pci::RasterSortRecord> merged;
        store.forEachMerged(
            [&](const pci::RasterSortRecord record) {
                merged.push_back(record);
            },
            {});
        CHECK(std::ranges::is_sorted(merged));
        CHECK(merged.size() == 6);

        std::vector<pci::RasterSortRecord> slice;
        store.forEachMergedAddressRange(
            4,
            10,
            [&](const pci::RasterSortRecord record) {
                slice.push_back(record);
            },
            {});
        CHECK(slice == std::vector<pci::RasterSortRecord>{
                           {.address = 4, .destination = 3},
                           {.address = 5, .destination = 2},
                           {.address = 9, .destination = 0}});

        std::stop_source cancelled;
        cancelled.request_stop();
        CHECK_THROWS_AS(
            store.forEachMerged([](const auto &) {}, cancelled.get_token()),
            pci::RasterColorizeRunStoreCancelled);
    }
    for (const auto &path : paths) {
        CHECK_FALSE(std::filesystem::exists(path));
    }
}

TEST_CASE("raster colorize run files preserve the independent 12-byte format",
          "[unit][scene][colorize][characterization][golden]")
{
    TemporaryDirectory directory;
    pci::LocalRasterColorizeRunStore store(directory.path());
    const std::array records{
        pci::RasterSortRecord{.address = 0, .destination = 0},
        pci::RasterSortRecord{.address = 0x0807060504030201ULL,
                              .destination = 0x0c0b0a09U},
    };
    store.writeSortedRun(records, directory.scratch, {});
    const auto paths = store.paths();
    REQUIRE(paths.size() == 1);

    std::ifstream input(paths.front(), std::ios::binary);
    REQUIRE(input.is_open());
    const std::vector<unsigned char> actual{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
    const std::vector<unsigned char> expected{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    };
    CHECK(actual == expected);
    CHECK(actual.size() == records.size() * 12);
}

TEST_CASE("raster colorize run store refuses unsorted input",
          "[unit][scene][colorize]")
{
    TemporaryDirectory directory;
    pci::LocalRasterColorizeRunStore store(directory.path());
    const std::array records{
        pci::RasterSortRecord{.address = 2, .destination = 0},
        pci::RasterSortRecord{.address = 1, .destination = 0},
    };
    CHECK_THROWS(store.writeSortedRun(records, directory.scratch, {}));
    CHECK(store.runCount() == 0);
}

TEST_CASE("local run-store full sink uses one call per whole-record chunk",
          "[unit][storage][colorize][characterization]")
{
    TemporaryDirectory directory;
    auto state = std::make_shared<RecordingFileState>();
    pci::LocalRasterColorizeRunStore store(directory.path(),
                                           recordingFileFactory(state));
    const std::array records{
        pci::RasterSortRecord{.address = 1, .destination = 4},
        pci::RasterSortRecord{.address = 5, .destination = 2},
        pci::RasterSortRecord{.address = 9, .destination = 0},
    };

    store.writeSortedRun(records, directory.scratch, {});

    CHECK(state->writes == 1);
    CHECK(state->flushes == 1);
    CHECK(state->closes == 1);
    CHECK(state->bytes.size() == records.size() * 12U);
    CHECK(store.runCount() == 1);
    CHECK(store.recordCount() == records.size());
    CHECK(store.bytesWritten() == records.size() * 12U);
}

TEST_CASE(
    "local run-store positive partial writes complete the remaining suffix",
    "[unit][storage][colorize][characterization][failure]")
{
    TemporaryDirectory directory;
    auto state = std::make_shared<RecordingFileState>();
    state->maximumWrite = 6;
    pci::LocalRasterColorizeRunStore store(directory.path(),
                                           recordingFileFactory(state));
    const std::array records{
        pci::RasterSortRecord{.address = 1, .destination = 4},
    };

    store.writeSortedRun(records, directory.scratch, {});
    CHECK(state->writes == 2);
    CHECK(state->flushes == 1);
    CHECK(state->closes == 1);
    CHECK(store.runCount() == 1);
    CHECK(store.recordCount() == 1);
    CHECK(store.bytesWritten() == 12);
}

} // namespace

TEST_CASE("run publication allocation failure leaves no private file",
          "[unit][colorize][publication][regression]")
{
    TemporaryDirectory directory;
    pci::LocalRasterColorizeRunStore store(directory.path());
    pci::LocalRasterColorizeRunStoreTestAccess::failPublication(store);
    const std::array records{
        pci::RasterSortRecord{.address = 1, .destination = 2}};
    CHECK_THROWS_AS(store.writeSortedRun(records, directory.scratch, {}),
                    std::bad_alloc);
    CHECK(store.runCount() == 0);
    CHECK(store.recordCount() == 0);
    CHECK(store.bytesWritten() == 0);
    CHECK(std::filesystem::is_empty(
        pci::LocalRasterColorizeRunStoreTestAccess::directory(store)));
}

TEST_CASE(
    "run chunk boundaries preserve independent bytes and exact write counts",
    "[unit][colorize][complexity][golden]")
{
    // Independent encodings, including duplicate addresses in destination
    // order.
    const std::array recordsPattern{
        pci::RasterSortRecord{0, 0},
        pci::RasterSortRecord{0x0807060504030201ULL, 0},
        pci::RasterSortRecord{0x0807060504030201ULL, 0x0c0b0a09U},
        pci::RasterSortRecord{0xffffffffffffffffULL, 0xffffffffU}};
    const std::array<std::array<unsigned char, 12>, 4> bytesPattern{
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
         {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0},
         {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
         {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}};
    for (std::size_t count : {0U, 1U, 5460U, 5461U, 5462U, 10923U}) {
        for (std::size_t capacity : {12U, 65536U}) {
            TemporaryDirectory directory;
            std::vector<pci::RasterSortRecord> records;
            std::vector<unsigned char> expected;
            for (std::size_t i = 0; i < count; ++i) {
                const auto pattern = i * 4 / count;
                records.push_back(recordsPattern[pattern]);
                expected.insert(expected.end(),
                                bytesPattern[pattern].begin(),
                                bytesPattern[pattern].end());
            }
            pci::LocalRasterColorizeRunStore store(directory.path());
            auto scratch = std::span(directory.scratch).first(capacity);
            store.writeSortedRun(records, scratch, {});
            CHECK(store.recordCount() == count);
            CHECK(store.bytesWritten() == 12 * count);
            if (count == 0) {
                CHECK(store.paths().empty());
                continue;
            }
            std::ifstream file(store.paths().front(), std::ios::binary);
            const std::vector<unsigned char> actual{
                std::istreambuf_iterator<char>(file), {}};
            CHECK(actual == expected);
            for (std::size_t i = 0; i < count; ++i) {
                std::array<std::byte, 12> independent;
                std::transform(expected.begin() +
                                   static_cast<std::ptrdiff_t>(i * 12),
                               expected.begin() +
                                   static_cast<std::ptrdiff_t>((i + 1) * 12),
                               independent.begin(),
                               [](auto byte) {
                                   return static_cast<std::byte>(byte);
                               });
                CHECK(pci::deserializeRasterSortRecord(independent) ==
                      records[i]);
                CHECK(pci::serializeRasterSortRecord(records[i]) ==
                      independent);
            }
            auto state = std::make_shared<RecordingFileState>();
            pci::LocalRasterColorizeRunStore counted(
                directory.path(), recordingFileFactory(state));
            counted.writeSortedRun(records, scratch, {});
            CHECK(state->writes ==
                  (count + capacity / 12 - 1) / (capacity / 12));
            CHECK(state->flushes == 1);
        }
    }
}

TEST_CASE("run write errors and cancellation clean up unpublished files",
          "[unit][colorize][failure]")
{
    for (int failure = 0; failure < 8; ++failure) {
        TemporaryDirectory directory;
        auto state = std::make_shared<RecordingFileState>();
        if (failure == 0)
            state->maximumWrite = 0;
        if (failure == 1)
            state->writeFails = true;
        if (failure == 2)
            state->flushFails = true;
        if (failure == 3)
            state->stop.request_stop();
        if (failure == 4) {
            state->maximumWrite = 6;
            state->stopOnWrite = true;
        }
        if (failure == 5)
            state->stopOnClose = true;
        if (failure == 7) {
            state->maximumWrite = 6;
            state->failWriteNumber = 2;
        }
        pci::LocalRasterColorizeRunStore store(directory.path(),
                                               recordingFileFactory(state));
        const std::array records{pci::RasterSortRecord{1, 2},
                                 pci::RasterSortRecord{3, 4}};
        auto scratch =
            std::span(directory.scratch).first(failure == 6 ? 11 : 12);
        CHECK_THROWS_AS(
            store.writeSortedRun(records, scratch, state->stop.get_token()),
            pci::RasterColorizeRunStoreError);
        CHECK(store.runCount() == 0);
        CHECK(store.recordCount() == 0);
        CHECK(store.bytesWritten() == 0);
        CHECK(std::filesystem::is_empty(
            pci::LocalRasterColorizeRunStoreTestAccess::directory(store)));
        if (failure == 4)
            CHECK(state->writes == 1);
        if (failure == 7) {
            CHECK(state->writes == 2);
            CHECK(state->bytes.size() == 6);
        }
    }
}
