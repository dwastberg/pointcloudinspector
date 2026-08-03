#include "import/pdal/LeafPageWriter.h"

#include "import/PointCloudImport.h"
#include "import/pdal/MortonRunRecord.h"

#include <algorithm>
#include <fstream>
#include <queue>
#include <stdexcept>

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
    }

    [[nodiscard]] bool next(MortonRunRecord &record)
    {
        input_.read(reinterpret_cast<char *>(&record),
                    static_cast<std::streamsize>(sizeof(record)));
        if (input_) {
            return true;
        }
        if (input_.eof() && input_.gcount() == 0) {
            return false;
        }
        throw std::runtime_error("truncated local point sort run");
    }

private:
    std::ifstream input_;
};

} // namespace

struct LeafPageWriter::Impl {
    Impl(const std::filesystem::path &path,
         const LocalPointPageSourcePtr &pageSource)
        : output(path, std::ios::binary | std::ios::trunc)
        , source(pageSource)
    {
        if (!output) {
            throw std::runtime_error("could not create local point payload: " +
                                     path.string());
        }
    }

    void begin(const PointCloudNodeId id)
    {
        if (active) {
            throw std::logic_error("local point page already active");
        }
        active = LocalPointPageRecord{.id = id, .payloadOffset = bytesWritten};
    }

    void append(const PointSample &sample)
    {
        if (!active) {
            throw std::logic_error("no active local point page");
        }
        const auto bytes = encodeLocalPoint(sample);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("could not write local point payload");
        }
        active->payloadChecksum =
            localPointCrc32(bytes, active->payloadChecksum);
        extendBounds(
            active->tightBounds, sample.position, active->pointCount == 0);
        ++active->pointCount;
        active->payloadBytes += bytes.size();
        bytesWritten += bytes.size();
    }

    [[nodiscard]] LocalPointPageRecord
    finish(const std::uint64_t sourcePointCount,
           const std::optional<Bounds3d> hierarchyBounds = std::nullopt)
    {
        if (!active || active->pointCount == 0) {
            throw std::logic_error("cannot finish an empty local point page");
        }
        active->sourcePointCount = sourcePointCount;
        if (hierarchyBounds) {
            active->tightBounds = *hierarchyBounds;
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("could not flush local point payload");
        }
        LocalPointPageRecord result = *active;
        active.reset();
        source->publish(result);
        return result;
    }

    std::ofstream output;
    LocalPointPageSourcePtr source;
    std::optional<LocalPointPageRecord> active;
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
    impl_->begin(id);
    for (const PointSample &sample : samples) {
        impl_->append(sample);
    }
    return impl_->finish(sourcePointCount, hierarchyBounds);
}

std::vector<LocalPointPageRecord>
LeafPageWriter::mergeRuns(const std::vector<std::filesystem::path> &runPaths,
                          const std::uint8_t level,
                          const std::uint32_t pointsPerLeaf,
                          const std::stop_token stopToken,
                          const std::function<void()> &heartbeat)
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
    std::uint64_t leafIndex = 0;
    std::uint64_t activePoints = 0;
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
        if (activePoints == 0) {
            impl_->begin(nodeIdFromMorton(leafIndex, level));
        }
        impl_->append(item.record.sample);
        ++activePoints;
        if (activePoints == pointsPerLeaf) {
            pages.push_back(impl_->finish(activePoints));
            activePoints = 0;
            ++leafIndex;
        }
        MortonRunRecord next;
        if (readers[item.run].next(next)) {
            queue.push({.record = next, .run = item.run});
        }
    }
    if (activePoints != 0) {
        pages.push_back(impl_->finish(activePoints));
    }
    return pages;
}

void LeafPageWriter::close()
{
    if (impl_->active) {
        throw std::logic_error("unfinished local point page");
    }
    impl_->output.flush();
    impl_->output.close();
    if (!impl_->output) {
        throw std::runtime_error("could not close local point payload");
    }
}

std::uint64_t LeafPageWriter::bytesWritten() const noexcept
{
    return impl_->bytesWritten;
}

} // namespace pci::local_index
