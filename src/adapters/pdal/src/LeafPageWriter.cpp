#include "LeafPageWriter.h"

#include "MortonRunRecord.h"
#include <pci/adapters/platform/QtPath.h>
#include <pci/operations/PointCloudImport.h>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QIODeviceBase>

#include <algorithm>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <vector>

namespace pci::local_index {
namespace {

void extendBounds(Bounds3d &bounds,
                  const Vec3d point,
                  const bool first) noexcept
{
    if (first) {
        bounds.minimum = {point.x, point.y, point.z};
        bounds.maximum = bounds.minimum;
        return;
    }
    bounds.minimum[0] = std::min(bounds.minimum[0], point.x);
    bounds.minimum[1] = std::min(bounds.minimum[1], point.y);
    bounds.minimum[2] = std::min(bounds.minimum[2], point.z);
    bounds.maximum[0] = std::max(bounds.maximum[0], point.x);
    bounds.maximum[1] = std::max(bounds.maximum[1], point.y);
    bounds.maximum[2] = std::max(bounds.maximum[2], point.z);
}

PointCloudNodeId nodeIdFromMorton(const std::uint64_t morton,
                                  const std::uint8_t level) noexcept
{
    PointCloudNodeId result{.level = level};
    for (std::uint8_t bit = 0; bit < level; ++bit) {
        result.x |= static_cast<std::uint32_t>((morton >> (bit * 3U)) & 1U)
                    << bit;
        result.y |= static_cast<std::uint32_t>((morton >> (bit * 3U + 1U)) & 1U)
                    << bit;
        result.z |= static_cast<std::uint32_t>((morton >> (bit * 3U + 2U)) & 1U)
                    << bit;
    }
    return result;
}

class RunReader final {
public:
    explicit RunReader(const std::filesystem::path &path)
        : input_(path, std::ios::binary)
    {
        if (!input_) {
            throw std::runtime_error("could not open local point sort run");
        }
        buffer_.resize(4096);
    }

    [[nodiscard]] bool next(MortonRunRecord &record)
    {
        if (offset_ >= count_) {
            input_.read(
                reinterpret_cast<char *>(buffer_.data()),
                static_cast<std::streamsize>(buffer_.size() * sizeof(MortonRunRecord)));
            const auto bytesRead = input_.gcount();
            if (bytesRead == 0) {
                return false;
            }
            if (bytesRead % static_cast<std::streamsize>(sizeof(MortonRunRecord)) != 0) {
                throw std::runtime_error("truncated local point sort run");
            }
            count_ = static_cast<std::size_t>(bytesRead / sizeof(MortonRunRecord));
            offset_ = 0;
        }
        record = buffer_[offset_++];
        return true;
    }

private:
    std::ifstream input_;
    std::vector<MortonRunRecord> buffer_;
    std::size_t offset_ = 0;
    std::size_t count_ = 0;
};

} // namespace

struct LeafPageWriter::Impl {
    Impl(const std::filesystem::path &path,
         const LocalPointPageSourcePtr &pageSource)
        : output(pathToQString(path))
        , source(pageSource)
    {
        if (!output.open(QIODeviceBase::WriteOnly | QIODeviceBase::NewOnly,
                         QFile::ReadOwner | QFile::WriteOwner)) {
            throw std::runtime_error("could not create local point payload: " +
                                     path.string());
        }
    }

    [[nodiscard]] LocalPointPageRecord
    appendPage(const PointCloudNodeId id,
               const std::span<const PointSample> samples,
               const std::uint64_t sourcePointCount,
               const std::optional<Bounds3d> hierarchyBounds = std::nullopt)
    {
        if (samples.empty()) {
            throw std::logic_error("cannot write an empty local point page");
        }
        Bounds3d bounds;
        bool first = true;
        for (const PointSample &sample : samples) {
            extendBounds(bounds, sample.position, first);
            first = false;
        }
        const Bounds3d effectiveBounds = hierarchyBounds.value_or(bounds);
        const double extent = std::max(effectiveBounds.maximumExtent(), 1e-9);
        const double scale = blockScaleForEdge(extent);
        const double invScale = scale > 0.0 ? 1.0 / scale : 0.0;
        const Vec3d origin{
            effectiveBounds.minimum[0],
            effectiveBounds.minimum[1],
            effectiveBounds.minimum[2],
        };

        pageBuffer_.resize(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const PointSample &s = samples[i];
            const auto q = quantizeToBlockFast(s.position, origin, invScale);
            pageBuffer_[i] = GpuPoint{
                .x = q[0],
                .y = q[1],
                .z = q[2],
                .attributes = s.packedAttributes,
                .rgba = s.rgba,
                .packedProperties = s.packedProperties,
            };
        }

        const auto byteSpan = std::as_bytes(std::span(pageBuffer_));
        const QByteArray digest = QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(byteSpan.data()),
                           static_cast<qsizetype>(byteSpan.size())),
            QCryptographicHash::Sha256);
        LocalPointPayloadDigest pageDigest{};
        std::ranges::transform(
            digest, pageDigest.begin(), [](const char val) {
                return static_cast<std::uint8_t>(static_cast<unsigned char>(val));
            });

        const std::uint64_t offset = bytesWritten;
        const qint64 bytesToWrite = static_cast<qint64>(byteSpan.size());
        if (output.write(reinterpret_cast<const char *>(byteSpan.data()),
                         bytesToWrite) != bytesToWrite) {
            throw std::runtime_error("could not write local point payload");
        }
        if (!output.flush()) {
            throw std::runtime_error("could not flush local point payload");
        }
        bytesWritten += static_cast<std::uint64_t>(bytesToWrite);

        LocalPointPageRecord record{
            .id = id,
            .tightBounds = effectiveBounds,
            .sourcePointCount = sourcePointCount,
            .pointCount = samples.size(),
            .payloadOffset = offset,
            .payloadBytes = static_cast<std::uint64_t>(bytesToWrite),
            .payloadDigest = pageDigest,
        };
        source->publish(record);
        return record;
    }

    QFile output;
    LocalPointPageSourcePtr source;
    std::vector<GpuPoint> pageBuffer_;
    std::uint64_t bytesWritten = 0;
};

LeafPageWriter::LeafPageWriter(const std::filesystem::path &payloadPath,
                               const LocalPointPageSourcePtr &source)
    : impl_(std::make_unique<Impl>(payloadPath, source))
{
}

LeafPageWriter::~LeafPageWriter() = default;
LeafPageWriter::LeafPageWriter(LeafPageWriter &&) noexcept = default;
LeafPageWriter &LeafPageWriter::operator=(LeafPageWriter &&) noexcept = default;

LocalPointPageRecord
LeafPageWriter::appendPage(const PointCloudNodeId id,
                           const std::span<const PointSample> samples,
                           const std::uint64_t sourcePointCount,
                           const std::optional<Bounds3d> hierarchyBounds)
{
    return impl_->appendPage(id, samples, sourcePointCount, hierarchyBounds);
}

std::vector<LocalPointPageRecord>
LeafPageWriter::writeLeafPages(
    const std::span<const MortonRunRecord> records,
    const std::uint8_t level,
    const std::uint32_t pointsPerLeaf,
    const std::stop_token stopToken,
    const std::function<void()> &heartbeat,
    const OnLeafPage &onLeafPage)
{
    std::vector<LocalPointPageRecord> pages;
    std::vector<PointSample> leafSamples;
    leafSamples.reserve(pointsPerLeaf);
    std::uint64_t leafIndex = 0;

    for (std::size_t i = 0; i < records.size(); ++i) {
        if ((i & 0xffffU) == 0) {
            if (stopToken.stop_requested()) {
                throw PointCloudImportCancelled();
            }
            heartbeat();
        }
        leafSamples.push_back(records[i].sample);
        if (leafSamples.size() == pointsPerLeaf) {
            const PointCloudNodeId id = nodeIdFromMorton(leafIndex++, level);
            const auto record =
                impl_->appendPage(id, leafSamples, leafSamples.size());
            if (onLeafPage) {
                onLeafPage(record, leafSamples);
            }
            pages.push_back(record);
            leafSamples.clear();
        }
    }
    if (!leafSamples.empty()) {
        const PointCloudNodeId id = nodeIdFromMorton(leafIndex++, level);
        const auto record =
            impl_->appendPage(id, leafSamples, leafSamples.size());
        if (onLeafPage) {
            onLeafPage(record, leafSamples);
        }
        pages.push_back(record);
    }
    return pages;
}

std::vector<LocalPointPageRecord>
LeafPageWriter::mergeRuns(const std::vector<std::filesystem::path> &runPaths,
                          const std::uint8_t level,
                          const std::uint32_t pointsPerLeaf,
                          const std::stop_token stopToken,
                          const std::function<void()> &heartbeat,
                          const OnLeafPage &onLeafPage)
{
    struct Item {
        MortonRunRecord record;
        std::size_t run = 0;
    };
    const auto later = [](const Item &left, const Item &right) {
        return left.record.morton != right.record.morton
                   ? left.record.morton > right.record.morton
                   : left.record.ordinal > right.record.ordinal;
    };
    std::vector<RunReader> readers;
    readers.reserve(runPaths.size());
    std::priority_queue<Item, std::vector<Item>, decltype(later)> queue(later);
    for (std::size_t index = 0; index < runPaths.size(); ++index) {
        readers.emplace_back(runPaths[index]);
        MortonRunRecord first;
        if (readers.back().next(first)) {
            queue.push({.record = first, .run = index});
        }
    }

    std::vector<LocalPointPageRecord> pages;
    std::vector<PointSample> leafSamples;
    leafSamples.reserve(pointsPerLeaf);
    std::uint64_t leafIndex = 0;
    std::uint64_t visited = 0;

    while (!queue.empty()) {
        if ((visited++ & 0xffffU) == 0) {
            if (stopToken.stop_requested()) {
                throw PointCloudImportCancelled();
            }
            heartbeat();
        }
        Item item = queue.top();
        queue.pop();
        leafSamples.push_back(item.record.sample);
        if (leafSamples.size() == pointsPerLeaf) {
            const PointCloudNodeId id = nodeIdFromMorton(leafIndex++, level);
            const auto record =
                impl_->appendPage(id, leafSamples, leafSamples.size());
            if (onLeafPage) {
                onLeafPage(record, leafSamples);
            }
            pages.push_back(record);
            leafSamples.clear();
        }
        MortonRunRecord next;
        if (readers[item.run].next(next)) {
            queue.push({.record = next, .run = item.run});
        }
    }
    if (!leafSamples.empty()) {
        const PointCloudNodeId id = nodeIdFromMorton(leafIndex++, level);
        const auto record =
            impl_->appendPage(id, leafSamples, leafSamples.size());
        if (onLeafPage) {
            onLeafPage(record, leafSamples);
        }
        pages.push_back(record);
    }
    return pages;
}

void LeafPageWriter::close()
{
    if (!impl_->output.flush()) {
        throw std::runtime_error("could not flush local point payload");
    }
    impl_->output.close();
    if (impl_->output.error() != QFileDevice::NoError) {
        throw std::runtime_error("could not close local point payload");
    }
}

std::uint64_t LeafPageWriter::bytesWritten() const noexcept
{
    return impl_->bytesWritten;
}

} // namespace pci::local_index
