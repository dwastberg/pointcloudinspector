#include "import/pdal/PdalPointCloudLoader.h"

#include "foundation/CheckedArithmetic.h"
#include "import/local/LocalPointIndexBuilder.h"
#include "import/pdal/PdalHierarchicalPointSource.h"
#include "import/pdal/PdalPointMapping.h"
#include "import/pdal/PdalSourceInspector.h"
#include "scene/BlockPartitioner.h"

#include <pdal/Options.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/filters/StreamCallbackFilter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace pci {
namespace {

constexpr std::uint64_t decodedBytesPerPoint =
    sizeof(GpuPoint) + sizeof(PointAttributes);
constexpr std::uint64_t desiredResidentNodes = 16;
constexpr std::uint64_t streamingPublicationInterval = maximumPointsPerBlock;

bool hierarchicalDriver(const std::string_view driver) noexcept
{
    return driver == "readers.copc" || driver == "readers.ept";
}

std::size_t pointsPerNodeForBudget(const std::uint64_t byteBudget)
{
    const std::uint64_t budgetedPoints =
        byteBudget / desiredResidentNodes / decodedBytesPerPoint;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(
        budgetedPoints, 1, PdalHierarchicalPointSource::defaultPointsPerNode));
}

std::uint64_t estimatedDecodeAllowance(const std::uint64_t pointCount) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(PointSample) + sizeof(GpuPoint) + sizeof(PointAttributes);
    return saturatingMultiply(pointCount, bytesPerPoint);
}

class SpatialPreviewSampler final {
public:
    SpatialPreviewSampler(const Bounds3d &bounds,
                          const std::uint64_t maximumPoints)
        : bounds_(bounds)
    {
        if (maximumPoints == 0) {
            throw std::invalid_argument(
                "spatial preview requires at least one point");
        }
        while (true) {
            std::optional<std::size_t> selectedAxis;
            const std::uint64_t cells = counts_[0] * counts_[1] * counts_[2];
            for (std::size_t axis = 0; axis < counts_.size(); ++axis) {
                const std::uint64_t nextCells =
                    cells / counts_[axis] * (counts_[axis] + 1);
                if (nextCells > maximumPoints) {
                    continue;
                }
                if (!selectedAxis || counts_[axis] < counts_[*selectedAxis]) {
                    selectedAxis = axis;
                }
            }
            if (!selectedAxis) {
                break;
            }
            ++counts_[*selectedAxis];
        }
        occupied_.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(maximumPoints, 65'536)));
    }

    [[nodiscard]] bool accept(const Vec3d position)
    {
        std::array<std::uint64_t, 3> index{};
        const std::array<double, 3> components{
            position.x, position.y, position.z};
        for (std::size_t axis = 0; axis < counts_.size(); ++axis) {
            const double extent = bounds_.maximum[axis] - bounds_.minimum[axis];
            const double normalized =
                extent > 0.0
                    ? (components[axis] - bounds_.minimum[axis]) / extent
                    : 0.0;
            index[axis] = static_cast<std::uint64_t>(std::clamp(
                std::floor(normalized * static_cast<double>(counts_[axis])),
                0.0,
                static_cast<double>(counts_[axis] - 1)));
        }
        const std::uint64_t key =
            index[0] + counts_[0] * (index[1] + counts_[1] * index[2]);
        return occupied_.insert(key).second;
    }

private:
    Bounds3d bounds_;
    std::array<std::uint64_t, 3> counts_{1, 1, 1};
    std::unordered_set<std::uint64_t> occupied_;
};

} // namespace

PointCloudImportPreflight
PdalPointCloudLoader::inspect(const PointCloudLoadOptions &options) const
{
    return inspect(options, defaultPointCloudDecodedByteBudget, {});
}

PointCloudImportPreflight
PdalPointCloudLoader::inspect(const PointCloudLoadRequest &request,
                              const std::stop_token stopToken) const
{
    return inspect(
        request.options, request.resources.decodedByteBudget, stopToken);
}

PointCloudScenePtr
PdalPointCloudLoader::load(const PointCloudLoadOptions &options,
                           const PointCloudLoadResources &resources,
                           const PointCloudLoadContext &context) const
{
    const PointCloudImportPreflight preflight =
        inspect(options, resources.decodedByteBudget, context.stopToken);
    return load(options, resources, preflight, context);
}

PointCloudScenePtr
PdalPointCloudLoader::load(const PointCloudLoadRequest &request,
                           const PointCloudLoadContext &context) const
{
    return load(request.options, request.resources, context);
}

PointCloudImportPreflight
PdalPointCloudLoader::inspect(const PointCloudLoadOptions &options,
                              const std::uint64_t decodedByteBudget,
                              const std::stop_token stopToken) const
{
    if (stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }
    PdalSourceInspector inspector;
    PointCloudMetadata metadata = inspector.inspect(options.sourcePath);
    if (stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }

    const bool nativeHierarchy = hierarchicalDriver(metadata.sourceDriver);
    const std::uint64_t desired =
        std::min(metadata.sourcePointCount, options.maximumPoints);
    const bool localPaging =
        !nativeHierarchy && metadata.sourceDriver == "readers.las" &&
        metadata.sourcePointCount > options.localPaging.pointThreshold;
    std::error_code error;
    const std::uintmax_t fileBytes =
        std::filesystem::file_size(options.sourcePath, error);
    const std::uint64_t safeFileBytes =
        error || fileBytes > std::numeric_limits<std::uint64_t>::max()
            ? 0
            : static_cast<std::uint64_t>(fileBytes);
    error.clear();
    const auto modificationTime =
        std::filesystem::last_write_time(options.sourcePath, error);
    const std::uint64_t modificationTicks =
        error ? 0
              : static_cast<std::uint64_t>(
                    modificationTime.time_since_epoch().count());
    const std::uint64_t rootPoints = std::min<std::uint64_t>(
        desired, pointsPerNodeForBudget(decodedByteBudget));
    return {
        .metadata = std::move(metadata),
        .sourceFileBytes = safeFileBytes,
        .sourceModificationTime = modificationTicks,
        .desiredRetainedPoints = desired,
        .estimatedResidentBytes = nativeHierarchy || localPaging
                                      ? estimatedFlatResidentBytes(rootPoints)
                                      : estimatedFlatResidentBytes(desired),
        .estimatedActiveBytes =
            nativeHierarchy || localPaging
                ? std::min(estimatedDecodeAllowance(rootPoints),
                           flatImportWorkingBytes)
                : flatImportWorkingBytes,
        .hierarchical = nativeHierarchy || localPaging,
        .localPaging = localPaging,
    };
}

PointCloudScenePtr
PdalPointCloudLoader::load(const PointCloudLoadOptions &options,
                           const PointCloudLoadResources &resources,
                           const PointCloudImportPreflight &preflight,
                           const PointCloudLoadContext &context) const
{
    if (options.maximumPoints == 0) {
        throw PointCloudImportError("maximumPoints must be greater than zero");
    }
    if (resources.decodedByteBudget <
        decodedBytesPerPoint * desiredResidentNodes) {
        throw PointCloudImportError(
            "decodedByteBudget is too small for the hierarchical cache");
    }
    if (context.stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }

    const PointCloudMetadata metadata = preflight.metadata;
    const std::uint64_t retainedPointLimit =
        preflight.retainedPointLimit == 0
            ? options.maximumPoints
            : std::min(options.maximumPoints, preflight.retainedPointLimit);
    if (retainedPointLimit == 0) {
        throw PointCloudImportError(
            "retainedPointLimit must be greater than zero");
    }
    const std::uint64_t stride = std::max<std::uint64_t>(
        1,
        (metadata.sourcePointCount + retainedPointLimit - 1) /
            retainedPointLimit);
    const std::uint64_t expectedPoints =
        std::min(metadata.sourcePointCount, retainedPointLimit);
    const std::uint64_t publicationInterval =
        preflight.spatialPreview
            ? std::clamp<std::uint64_t>(
                  retainedPointLimit / 8, 1, streamingPublicationInterval)
            : streamingPublicationInterval;

    if (hierarchicalDriver(metadata.sourceDriver)) {
        if (context.progress) {
            context.progress({
                .stage = PointCloudImportStage::Reading,
                .processed = 0,
                .total = metadata.sourcePointCount,
            });
        }
        try {
            auto source = std::make_shared<PdalHierarchicalPointSource>(
                metadata,
                options.maximumPoints,
                pointsPerNodeForBudget(resources.decodedByteBudget));
            HierarchyResidencyCoordinator::ParticipantPtr rootParticipant;
            std::optional<HierarchyResidencyCoordinator::DecodeLease>
                rootDecodeLease;
            if (resources.residency) {
                rootParticipant =
                    resources.residency->registerTransientDecode();
                rootDecodeLease = rootParticipant->acquireDecode(
                    context.stopToken,
                    context.stopToken,
                    estimatedDecodeAllowance(
                        source->rootNode().estimatedPointCount));
                if (!rootDecodeLease) {
                    throw PointCloudImportCancelled();
                }
            }
            PointCloudNodePayloadPtr root =
                source->loadNode(rootPointCloudNode, context.stopToken);
            rootDecodeLease.reset();
            rootParticipant.reset();
            auto scene =
                std::make_shared<PointCloudScene>(metadata,
                                                  std::move(source),
                                                  std::move(root),
                                                  resources.decodedByteBudget);
            if (context.sceneReady) {
                context.sceneReady(scene);
            }
            if (context.progress) {
                context.progress({
                    .stage = PointCloudImportStage::Reading,
                    .processed = metadata.sourcePointCount,
                    .total = metadata.sourcePointCount,
                });
            }
            scene->markLoadingComplete();
            return scene;
        } catch (const PointCloudDataSourceCancelled &) {
            throw PointCloudImportCancelled();
        } catch (const PointCloudImportCancelled &) {
            throw;
        } catch (const std::exception &error) {
            throw PointCloudImportError(
                "Could not open hierarchical point cloud '" +
                options.sourcePath.string() + "': " + error.what());
        }
    }

    if (preflight.localPaging) {
        if (options.localPaging.cacheDirectory.empty()) {
            throw PointCloudImportError(
                "Local paging requires a writable application cache directory");
        }
        try {
            LocalPointIndexBuilder builder;
            PointCloudScenePtr scene;
            const LocalPointPageBuildResult result = builder.openOrBuild(
                preflight,
                options.maximumPoints,
                {
                    .cacheDirectory = options.localPaging.cacheDirectory,
                    .pointsPerLeaf = options.localPaging.pagePoints,
                    .rootPreviewPoints = options.localPaging.rootPreviewPoints,
                    .sortMemoryBytes = options.localPaging.sortMemoryBytes,
                    .diskCacheBytes = options.localPaging.diskCacheBytes,
                },
                context.stopToken,
                context.progress,
                [&](const LocalPointPageSourcePtr &source,
                    const PointCloudNodePayloadPtr &root) {
                    if (scene) {
                        return;
                    }
                    scene = std::make_shared<PointCloudScene>(
                        metadata,
                        source,
                        root,
                        resources.decodedByteBudget,
                        source->committed());
                    if (context.sceneReady) {
                        context.sceneReady(scene);
                    }
                });
            if (!scene) {
                scene = std::make_shared<PointCloudScene>(
                    metadata,
                    result.source,
                    result.rootPayload,
                    resources.decodedByteBudget);
                if (context.sceneReady) {
                    context.sceneReady(scene);
                }
            }
            scene->markLoadingComplete();
            return scene;
        } catch (const PointCloudImportCancelled &) {
            throw;
        } catch (const std::exception &error) {
            throw PointCloudImportError(
                "Could not build/open local pages for '" +
                options.sourcePath.string() + "': " + error.what());
        }
    }

    auto scene = std::make_shared<PointCloudScene>(metadata);
    if (resources.flatReservation) {
        scene->setResidentMemoryReservation(resources.flatReservation);
    }
    BlockPartitioner partitioner(
        metadata.sourceBounds, expectedPoints, [&scene](PointBlockPtr block) {
            scene->addBlock(std::move(block));
        });

    if (context.progress) {
        context.progress({
            .stage = PointCloudImportStage::Reading,
            .processed = 0,
            .total = metadata.sourcePointCount,
        });
    }
    if (context.sceneReady) {
        context.sceneReady(scene);
    }

    try {
        pdal::StageFactory factory;
        pdal::Stage *reader = factory.createStage(metadata.sourceDriver);
        if (!reader) {
            throw PointCloudImportError("PDAL reader is unavailable: " +
                                        metadata.sourceDriver);
        }
        pdal::Options readerOptions;
        readerOptions.add("filename", options.sourcePath.string());
        reader->setOptions(readerOptions);

        std::uint64_t processed = 0;
        std::uint64_t sampled = 0;
        std::optional<SpatialPreviewSampler> spatialSampler;
        if (preflight.spatialPreview &&
            retainedPointLimit < metadata.sourcePointCount) {
            spatialSampler.emplace(metadata.sourceBounds, retainedPointLimit);
        }
        pdal::StreamCallbackFilter callback;
        callback.setInput(*reader);
        callback.setCallback([&](pdal::PointRef &point) {
            if (context.stopToken.stop_requested()) {
                throw PointCloudImportCancelled();
            }
            bool added = false;
            if (spatialSampler) {
                const PointSample sample = mapPdalPoint(point, metadata);
                if (spatialSampler->accept(sample.position) &&
                    sampled < retainedPointLimit) {
                    partitioner.add(sample);
                    ++sampled;
                    added = true;
                }
            } else if (processed % stride == 0 &&
                       sampled < retainedPointLimit) {
                partitioner.add(mapPdalPoint(point, metadata));
                ++sampled;
                added = true;
            }
            if (added && sampled % publicationInterval == 0) {
                // Dense cells seal themselves at the block threshold. For
                // spatially scattered input, publish only the largest
                // remaining cell so periodic latency stays bounded without
                // creating a buffer for every tiny active cell.
                static_cast<void>(partitioner.flushLargest(1));
            }
            ++processed;
            if (context.progress && processed % 65'536 == 0) {
                context.progress({
                    .stage = PointCloudImportStage::Reading,
                    .processed = processed,
                    .total = metadata.sourcePointCount,
                });
            }
            return true;
        });

        pdal::FixedPointTable table(4096);
        callback.prepare(table);
        if (!callback.pipelineStreamable()) {
            throw PointCloudImportError(
                "PDAL pipeline is not streamable for '" +
                options.sourcePath.string() + "'");
        }
        callback.execute(table);
        partitioner.finish();
        scene->markLoadingComplete();

        if (resources.flatReservation && !resources.flatReservation->tryResize(
                                             scene->decodedResidentBytes())) {
            throw PointCloudImportError(
                "Flat point payload exceeded its admitted memory reservation");
        }

        if (context.progress) {
            context.progress({
                .stage = PointCloudImportStage::Reading,
                .processed = processed,
                .total = metadata.sourcePointCount,
            });
        }
    } catch (const PointCloudImportCancelled &) {
        throw;
    } catch (const PointCloudImportError &) {
        throw;
    } catch (const std::exception &error) {
        throw PointCloudImportError("Could not load point cloud '" +
                                    options.sourcePath.string() +
                                    "': " + error.what());
    }

    return scene;
}

} // namespace pci
