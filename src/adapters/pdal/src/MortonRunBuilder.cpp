#include "MortonRunBuilder.h"

#include "MortonRunRecord.h"
#include <pci/adapters/platform/QtPath.h>

#include <QFile>
#include <QIODeviceBase>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

namespace pci::local_index {
namespace {

consteval auto makeMortonTable10()
{
    std::array<std::uint64_t, 1024> table{};
    for (std::uint32_t i = 0; i < 1024; ++i) {
        std::uint64_t r = 0;
        for (std::uint32_t b = 0; b < 10; ++b) {
            r |= static_cast<std::uint64_t>((i >> b) & 1U) << (b * 3U);
        }
        table[i] = r;
    }
    return table;
}

constexpr auto mortonTable10 = makeMortonTable10();

inline std::uint64_t dilate20(const std::uint32_t value) noexcept
{
    return mortonTable10[value & 0x3ffU] |
           (mortonTable10[(value >> 10U) & 0x3ffU] << 30U);
}

std::uint32_t spatialIndexFast(const double value,
                               const double minimum,
                               const double invExtent,
                               const std::uint32_t cells) noexcept
{
    const double normalized =
        value >= minimum ? (value - minimum) * invExtent : 0.0;
    return static_cast<std::uint32_t>(
        std::clamp(std::floor(normalized * static_cast<double>(cells)),
                   0.0,
                   static_cast<double>(cells - 1U)));
}

std::uint64_t mortonCode(const Vec3d point,
                         const Bounds3d &bounds,
                         const std::array<double, 3> &invExtents,
                         const std::uint8_t level) noexcept
{
    const std::uint32_t cells = std::uint32_t{1} << level;
    const std::uint32_t cx =
        spatialIndexFast(point.x, bounds.minimum[0], invExtents[0], cells);
    const std::uint32_t cy =
        spatialIndexFast(point.y, bounds.minimum[1], invExtents[1], cells);
    const std::uint32_t cz =
        spatialIndexFast(point.z, bounds.minimum[2], invExtents[2], cells);
    return dilate20(cx) | (dilate20(cy) << 1U) | (dilate20(cz) << 2U);
}

void sortMortonRecords(std::vector<MortonRunRecord> &records)
{
    const std::size_t n = records.size();
    if (n < 64) {
        std::ranges::sort(records, [](const auto &left, const auto &right) {
            return left.morton != right.morton ? left.morton < right.morton
                                               : left.ordinal < right.ordinal;
        });
        return;
    }
    std::vector<MortonRunRecord> auxiliary(n);
    std::array<std::uint32_t, 1024> counts{};

    MortonRunRecord *src = records.data();
    MortonRunRecord *dst = auxiliary.data();

    // 6 passes of 10 bits = 60 bits
    for (std::uint32_t pass = 0; pass < 6; ++pass) {
        const std::uint32_t shift = pass * 10U;
        counts.fill(0);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t bucket =
                static_cast<std::uint32_t>((src[i].morton >> shift) & 0x3ffU);
            ++counts[bucket];
        }
        if (counts[0] == n) {
            continue;
        }
        std::uint32_t total = 0;
        for (std::size_t i = 0; i < 1024; ++i) {
            const std::uint32_t count = counts[i];
            counts[i] = total;
            total += count;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t bucket =
                static_cast<std::uint32_t>((src[i].morton >> shift) & 0x3ffU);
            dst[counts[bucket]++] = src[i];
        }
        std::swap(src, dst);
    }
    if (src != records.data()) {
        std::memcpy(records.data(), src, n * sizeof(MortonRunRecord));
    }
}

void writeRun(const std::filesystem::path &path,
              std::vector<MortonRunRecord> &records)
{
    sortMortonRecords(records);
    QFile output(pathToQString(path));
    if (!output.open(QIODeviceBase::WriteOnly | QIODeviceBase::NewOnly,
                     QFile::ReadOwner | QFile::WriteOwner)) {
        throw std::runtime_error("could not create local point sort run");
    }
    const std::size_t byteCount = records.size() * sizeof(MortonRunRecord);
    if (output.write(reinterpret_cast<const char *>(records.data()),
                     static_cast<qint64>(byteCount)) !=
            static_cast<qint64>(byteCount) ||
        !output.flush()) {
        throw std::runtime_error("could not write local point sort run");
    }
    records.clear();
}

} // namespace

struct MortonRunBuilder::Impl {
    std::filesystem::path directory;
    Bounds3d bounds;
    std::array<double, 3> invExtents{};
    std::uint8_t level = 0;
    std::size_t capacity = 0;
    std::vector<MortonRunRecord> chunk;
    std::vector<std::filesystem::path> runs;
    bool inMemorySorted = false;
};

MortonRunBuilder::MortonRunBuilder(std::filesystem::path runDirectory,
                                   const std::uint64_t memoryBytes,
                                   const Bounds3d bounds,
                                   const std::uint8_t mortonLevel)
    : impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(runDirectory);
    impl_->bounds = bounds;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double extent = bounds.maximum[axis] - bounds.minimum[axis];
        impl_->invExtents[axis] = extent > 0.0 ? 1.0 / extent : 0.0;
    }
    impl_->level = mortonLevel;
    impl_->capacity = static_cast<std::size_t>(
        std::max<std::uint64_t>(1, memoryBytes / sizeof(MortonRunRecord)));
    impl_->chunk.reserve(impl_->capacity);
}

MortonRunBuilder::~MortonRunBuilder() = default;
MortonRunBuilder::MortonRunBuilder(MortonRunBuilder &&) noexcept = default;
MortonRunBuilder &
MortonRunBuilder::operator=(MortonRunBuilder &&) noexcept = default;

void MortonRunBuilder::add(const PointSample &sample,
                           const std::uint64_t ordinal)
{
    impl_->chunk.push_back({
        .morton = mortonCode(
            sample.position, impl_->bounds, impl_->invExtents, impl_->level),
        .ordinal = ordinal,
        .sample = sample,
    });
    if (impl_->chunk.size() >= impl_->capacity) {
        const std::filesystem::path run =
            impl_->directory /
            ("run-" + std::to_string(impl_->runs.size()) + ".bin");
        writeRun(run, impl_->chunk);
        impl_->runs.push_back(run);
    }
}

void MortonRunBuilder::finish()
{
    if (impl_->chunk.empty()) {
        return;
    }
    if (impl_->runs.empty()) {
        // Fast path: everything fit within memoryBytes; sort directly in memory
        // with zero disk spill.
        sortMortonRecords(impl_->chunk);
        impl_->inMemorySorted = true;
        return;
    }
    const std::filesystem::path run =
        impl_->directory /
        ("run-" + std::to_string(impl_->runs.size()) + ".bin");
    writeRun(run, impl_->chunk);
    impl_->runs.push_back(run);
}

const std::vector<std::filesystem::path> &
MortonRunBuilder::runs() const noexcept
{
    return impl_->runs;
}

std::span<const MortonRunRecord>
MortonRunBuilder::inMemoryRecords() const noexcept
{
    return impl_->inMemorySorted ? std::span<const MortonRunRecord>(impl_->chunk)
                                 : std::span<const MortonRunRecord>();
}

std::size_t MortonRunBuilder::recordBytes() noexcept
{
    return sizeof(MortonRunRecord);
}

} // namespace pci::local_index
