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

std::uint32_t spatialIndex(const double value,
                           const double minimum,
                           const double maximum,
                           const std::uint32_t cells) noexcept
{
    const double extent = maximum - minimum;
    const double normalized = extent > 0.0 ? (value - minimum) / extent : 0.0;
    return static_cast<std::uint32_t>(
        std::clamp(std::floor(normalized * static_cast<double>(cells)),
                   0.0,
                   static_cast<double>(cells - 1U)));
}

std::uint64_t mortonCode(const Vec3d point,
                         const Bounds3d &bounds,
                         const std::uint8_t level) noexcept
{
    const std::uint32_t cells = std::uint32_t{1} << level;
    const std::array<std::uint32_t, 3> coordinates{
        spatialIndex(point.x, bounds.minimum[0], bounds.maximum[0], cells),
        spatialIndex(point.y, bounds.minimum[1], bounds.maximum[1], cells),
        spatialIndex(point.z, bounds.minimum[2], bounds.maximum[2], cells),
    };
    std::uint64_t result = 0;
    for (std::uint8_t bit = 0; bit < level; ++bit) {
        result |= static_cast<std::uint64_t>((coordinates[0] >> bit) & 1U)
                  << (bit * 3U);
        result |= static_cast<std::uint64_t>((coordinates[1] >> bit) & 1U)
                  << (bit * 3U + 1U);
        result |= static_cast<std::uint64_t>((coordinates[2] >> bit) & 1U)
                  << (bit * 3U + 2U);
    }
    return result;
}

void writeRun(const std::filesystem::path &path,
              std::vector<MortonRunRecord> &records)
{
    std::ranges::sort(records, [](const auto &left, const auto &right) {
        return left.morton != right.morton ? left.morton < right.morton
                                           : left.ordinal < right.ordinal;
    });
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
    std::uint8_t level = 0;
    std::size_t capacity = 0;
    std::vector<MortonRunRecord> chunk;
    std::vector<std::filesystem::path> runs;
};

MortonRunBuilder::MortonRunBuilder(std::filesystem::path runDirectory,
                                   const std::uint64_t memoryBytes,
                                   const Bounds3d bounds,
                                   const std::uint8_t mortonLevel)
    : impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(runDirectory);
    impl_->bounds = bounds;
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
        .morton = mortonCode(sample.position, impl_->bounds, impl_->level),
        .ordinal = ordinal,
        .sample = sample,
    });
    if (impl_->chunk.size() >= impl_->capacity) {
        finish();
    }
}

void MortonRunBuilder::finish()
{
    if (impl_->chunk.empty()) {
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

std::size_t MortonRunBuilder::recordBytes() noexcept
{
    return sizeof(MortonRunRecord);
}

} // namespace pci::local_index
