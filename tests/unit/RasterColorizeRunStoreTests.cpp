#include "scene/RasterColorizeRunStore.h"

#include "scene/PointCloudDataSource.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <vector>

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

private:
    QTemporaryDir directory_;
    std::filesystem::path path_;
};

TEST_CASE("raster colorize run records have an exact little-endian format",
          "[unit][scene][colorize]")
{
    static_assert(sizeof(pci::RasterSortRecord) == 16);
    const pci::RasterSortRecord record{
        .address = std::numeric_limits<std::uint64_t>::max(),
        .destination = std::numeric_limits<std::uint32_t>::max(),
    };
    const auto bytes = pci::RasterColorizeRunStore::serialize(record);
    static_assert(bytes.size() == 12);
    CHECK(std::ranges::all_of(bytes, [](const std::byte value) {
        return value == std::byte{0xff};
    }));
    CHECK(pci::RasterColorizeRunStore::deserialize(bytes) == record);

    const pci::RasterSortRecord patterned{
        .address = 0x0807060504030201ULL,
        .destination = 0x0c0b0a09U,
    };
    CHECK(pci::RasterColorizeRunStore::serialize(patterned) ==
          std::array<std::byte, 12>{std::byte{0x01},
                                    std::byte{0x02},
                                    std::byte{0x03},
                                    std::byte{0x04},
                                    std::byte{0x05},
                                    std::byte{0x06},
                                    std::byte{0x07},
                                    std::byte{0x08},
                                    std::byte{0x09},
                                    std::byte{0x0a},
                                    std::byte{0x0b},
                                    std::byte{0x0c}});
}

TEST_CASE("raster colorize run store merges runs and cleans files",
          "[unit][scene][colorize]")
{
    TemporaryDirectory directory;
    std::vector<std::filesystem::path> paths;
    {
        pci::RasterColorizeRunStore store(directory.path());
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
        store.writeSortedRun(first);
        store.writeSortedRun(second);
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
            pci::PointCloudDataSourceCancelled);
    }
    for (const auto &path : paths) {
        CHECK_FALSE(std::filesystem::exists(path));
    }
}

TEST_CASE("raster colorize run store refuses unsorted input",
          "[unit][scene][colorize]")
{
    TemporaryDirectory directory;
    pci::RasterColorizeRunStore store(directory.path());
    const std::array records{
        pci::RasterSortRecord{.address = 2, .destination = 0},
        pci::RasterSortRecord{.address = 1, .destination = 0},
    };
    CHECK_THROWS(store.writeSortedRun(records));
    CHECK(store.runCount() == 0);
}

} // namespace
