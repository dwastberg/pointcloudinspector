#include <pci/runtime/scene/SceneRuntime.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pci {
namespace {

template <typename Binding>
[[nodiscard]] const Binding *bindingAs(const SceneRuntimeBinding &binding)
{
    return std::get_if<Binding>(&binding);
}

} // namespace

SceneRuntime::SceneRuntime(const std::uint64_t decodedByteBudget,
                           const std::size_t maximumConcurrentDecodes,
                           HierarchyDecodeAdmissionPtr decodeAdmission,
                           PointMemoryBudgetPtr memoryBudget,
                           DecodedPageCachePtr initialCache)
    : pointResidency_(
          std::make_shared<PointResidencyService>(decodedByteBudget,
                                                  maximumConcurrentDecodes,
                                                  std::move(decodeAdmission),
                                                  std::move(memoryBudget),
                                                  std::move(initialCache)))
{
}

SceneRuntimeSnapshot::SceneRuntimeSnapshot(
    std::unordered_map<BindingGeneration, SceneRuntimeBinding> bindings,
    PointResidencyServicePtr residency)
    : bindings_(std::move(bindings))
    , pointResidency_(std::move(residency))
{
}

SceneRuntime::SceneRuntime(PointResidencyServicePtr residency)
    : pointResidency_(std::move(residency))
{
}

SceneRuntime SceneRuntime::copyBindings() const
{
    SceneRuntime result(pointResidency_);
    result.bindings_ = bindings_;
    return result;
}

std::shared_ptr<PointDatasetRuntime>
SceneRuntimeSnapshot::point(const PointCloudSourceId sourceId,
                            const BindingGeneration generation) const
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return {};
    }
    const PointRuntimeBinding *binding =
        bindingAs<PointRuntimeBinding>(found->second);
    return binding && binding->descriptor.sourceId == sourceId
               ? binding->runtime
               : std::shared_ptr<PointDatasetRuntime>{};
}

RasterTileSourcePtr
SceneRuntimeSnapshot::raster(const RasterSourceId sourceId,
                             const BindingGeneration generation) const
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return {};
    }
    const RasterRuntimeBinding *binding =
        bindingAs<RasterRuntimeBinding>(found->second);
    return binding && binding->descriptor.sourceId == sourceId
               ? binding->source
               : RasterTileSourcePtr{};
}

std::optional<bool> SceneRuntimeSnapshot::pointActive(
    const PointCloudSourceId sourceId,
    const BindingGeneration generation) const noexcept
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return std::nullopt;
    }
    const PointRuntimeBinding *binding =
        bindingAs<PointRuntimeBinding>(found->second);
    return binding && binding->descriptor.sourceId == sourceId
               ? std::optional<bool>{binding->active}
               : std::nullopt;
}

std::size_t SceneRuntimeSnapshot::bindingCount() const noexcept
{
    return bindings_.size();
}

std::size_t SceneRuntimeSnapshot::pointBindingCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(bindings_, [](const auto &entry) {
            return std::holds_alternative<PointRuntimeBinding>(entry.second);
        }));
}

std::size_t SceneRuntimeSnapshot::rasterBindingCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(bindings_, [](const auto &entry) {
            return std::holds_alternative<RasterRuntimeBinding>(entry.second);
        }));
}

bool SceneRuntime::attachPoint(PointRuntimeBinding binding)
{
    if (!binding.runtime || binding.descriptor.sourceId.value() == 0 ||
        binding.generation.value() == 0) {
        throw std::invalid_argument("point runtime bindings require a source, "
                                    "identity, and generation");
    }
    if (binding.runtime->sourceId() != binding.descriptor.sourceId) {
        throw std::invalid_argument(
            "point runtime binding source identity does not match its runtime");
    }
    if (bindings_.contains(binding.generation) ||
        contains(binding.descriptor.sourceId)) {
        return false;
    }
    const BindingGeneration generation = binding.generation;
    const auto runtime = binding.runtime;
    const bool active = binding.active;
    const auto [position, inserted] =
        bindings_.emplace(generation, SceneRuntimeBinding{std::move(binding)});
    if (!inserted) {
        return false;
    }
    try {
        fitHierarchyRoots();
        runtime->setDocumentHierarchyResources(
            pointResidency_->coordinator(),
            pointResidency_->cache(),
            pointResidency_->scheduler(),
            active,
            pointResidency_->completionQueue());
        rebalanceHierarchyResidency();
    } catch (...) {
        bindings_.erase(position);
        runtime->useStandaloneHierarchyResidency();
        rebalanceHierarchyResidency();
        throw;
    }
    snapshotCache_.reset();
    return true;
}

bool SceneRuntime::stagePoint(PointRuntimeBinding binding)
{
    if (!binding.runtime || !binding.generation.value() ||
        !binding.descriptor.sourceId.value() ||
        binding.runtime->sourceId() != binding.descriptor.sourceId) {
        throw std::invalid_argument("invalid staged point binding");
    }
    if (bindings_.contains(binding.generation) ||
        contains(binding.descriptor.sourceId)) {
        return false;
    }
    bindings_.emplace(binding.generation, std::move(binding));
    snapshotCache_.reset();
    return true;
}

void SceneRuntime::preparePointAttachments()
{
    std::vector<PointRuntimeBinding> points;
    for (const auto &[generation, binding] : bindings_) {
        static_cast<void>(generation);
        if (const auto *point = bindingAs<PointRuntimeBinding>(binding);
            point && point->runtime->hierarchical()) {
            points.push_back(*point);
        }
    }
    std::ranges::sort(points, {}, &PointRuntimeBinding::generation);
    std::uint64_t minimum = 0;
    for (const auto &point : points) {
        minimum =
            saturatingAdd(minimum,
                          std::max(point.runtime->minimumRootPayloadBytes(),
                                   point.runtime->reservedRootBytes()));
    }
    auto available = memoryBudget()->availableBytes();
    for (const auto &point : points) {
        available =
            saturatingAdd(available, point.runtime->reservedRootBytes());
    }
    if (minimum > available) {
        throw std::length_error(
            "the runtime CPU budget cannot retain hierarchy previews");
    }
    preparedPoints_.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto &point = points[index];
        const auto allowance =
            std::max(point.runtime->minimumRootPayloadBytes(),
                     point.runtime->reservedRootBytes()) +
            (available - minimum) / points.size() +
            (index < (available - minimum) % points.size() ? 1 : 0);
        preparedPoints_.emplace_back(point.runtime,
                                     point.runtime->prepareAttachment(
                                         pointResidency_->coordinator(),
                                         pointResidency_->cache(),
                                         pointResidency_->scheduler(),
                                         point.active,
                                         allowance,
                                         pointResidency_->completionQueue()));
    }
    pointResidency_->syncCacheBudget();
    static_cast<void>(snapshot());
}

bool SceneRuntime::stageDetach(const BindingGeneration generation)
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return false;
    }
    if (const auto *point = bindingAs<PointRuntimeBinding>(found->second)) {
        preparedPoints_.emplace_back(
            point->runtime, point->runtime->prepareStandaloneAttachment());
    }
    bindings_.erase(found);
    snapshotCache_.reset();
    return true;
}

void SceneRuntime::preparePointDetachments(const SceneRuntime &previous)
{
    for (const auto &[generation, binding] : previous.bindings_) {
        if (const auto *point = bindingAs<PointRuntimeBinding>(binding);
            point && !bindings_.contains(generation)) {
            preparedPoints_.emplace_back(
                point->runtime, point->runtime->prepareStandaloneAttachment());
        }
    }
}

void SceneRuntime::commitPointAttachments() noexcept
{
    for (auto &[runtime, prepared] : preparedPoints_) {
        static_cast<void>(prepared);
        runtime->quiesceForAttachment();
    }
    for (auto &[runtime, prepared] : preparedPoints_) {
        runtime->commitAttachment(*prepared);
    }
    preparedPoints_.clear();
}

void SceneRuntime::resumePointRequests() noexcept
{
    for (const auto &[generation, binding] : bindings_) {
        static_cast<void>(generation);
        if (const auto *point = bindingAs<PointRuntimeBinding>(binding);
            point && point->runtime->hierarchical()) {
            try {
                point->runtime->requestNodes({});
            } catch (...) {
                point->runtime->publishInvalidation();
            }
        }
    }
}

bool SceneRuntime::attachRaster(RasterRuntimeBinding binding)
{
    if (!binding.source || binding.descriptor.sourceId.value() == 0 ||
        binding.generation.value() == 0) {
        throw std::invalid_argument("raster runtime bindings require a source, "
                                    "identity, and generation");
    }
    if (bindings_.contains(binding.generation) ||
        contains(binding.descriptor.sourceId)) {
        return false;
    }
    const BindingGeneration generation = binding.generation;
    const bool inserted =
        bindings_.emplace(generation, SceneRuntimeBinding{std::move(binding)})
            .second;
    if (inserted) {
        snapshotCache_.reset();
    }
    return inserted;
}

std::shared_ptr<PointDatasetRuntime>
SceneRuntime::point(const PointCloudSourceId sourceId,
                    const BindingGeneration generation) const
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return {};
    }
    const PointRuntimeBinding *binding =
        bindingAs<PointRuntimeBinding>(found->second);
    return binding && binding->descriptor.sourceId == sourceId
               ? binding->runtime
               : std::shared_ptr<PointDatasetRuntime>{};
}

RasterTileSourcePtr
SceneRuntime::raster(const RasterSourceId sourceId,
                     const BindingGeneration generation) const
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return {};
    }
    const RasterRuntimeBinding *binding =
        bindingAs<RasterRuntimeBinding>(found->second);
    return binding && binding->descriptor.sourceId == sourceId
               ? binding->source
               : RasterTileSourcePtr{};
}

bool SceneRuntime::setPointActive(const PointCloudSourceId sourceId,
                                  const BindingGeneration generation,
                                  const bool active)
{
    const PointRuntimeActivityUpdate update{
        .sourceId = sourceId,
        .generation = generation,
        .active = active,
    };
    return setPointActive({&update, 1});
}

bool SceneRuntime::setPointActive(
    const std::span<const PointRuntimeActivityUpdate> updates)
{
    std::unordered_set<BindingGeneration> uniqueGenerations;
    uniqueGenerations.reserve(updates.size());
    std::vector<PointRuntimeBinding *> bindings;
    bindings.reserve(updates.size());
    for (const PointRuntimeActivityUpdate &update : updates) {
        const auto found = bindings_.find(update.generation);
        PointRuntimeBinding *binding =
            found == bindings_.end()
                ? nullptr
                : std::get_if<PointRuntimeBinding>(&found->second);
        if (!binding || binding->descriptor.sourceId != update.sourceId ||
            !uniqueGenerations.insert(update.generation).second) {
            return false;
        }
        bindings.push_back(binding);
    }

    bool changed = false;
    for (std::size_t index = 0; index < updates.size(); ++index) {
        PointRuntimeBinding &binding = *bindings[index];
        const bool active = updates[index].active;
        if (binding.active == active) {
            continue;
        }
        binding.runtime->setHierarchyResidencyActive(active);
        binding.active = active;
        changed = true;
    }
    if (changed) {
        rebalanceHierarchyResidency();
        snapshotCache_.reset();
    }
    return true;
}

bool SceneRuntime::detach(const BindingGeneration generation)
{
    const auto found = bindings_.find(generation);
    if (found == bindings_.end()) {
        return false;
    }
    if (const PointRuntimeBinding *point =
            bindingAs<PointRuntimeBinding>(found->second)) {
        point->runtime->requestNodes({});
        point->runtime->useStandaloneHierarchyResidency();
    }
    bindings_.erase(found);
    snapshotCache_.reset();
    rebalanceHierarchyResidency();
    return true;
}

void SceneRuntime::clear()
{
    std::vector<std::shared_ptr<PointDatasetRuntime>> points;
    points.reserve(pointBindingCount());
    for (const auto &[generation, binding] : bindings_) {
        static_cast<void>(generation);
        if (const PointRuntimeBinding *point =
                bindingAs<PointRuntimeBinding>(binding)) {
            points.push_back(point->runtime);
        }
    }
    for (const auto &point : points) {
        point->requestNodes({});
        point->useStandaloneHierarchyResidency();
    }
    bindings_.clear();
    snapshotCache_.reset();
    rebalanceHierarchyResidency();
}

void SceneRuntime::swap(SceneRuntime &other) noexcept
{
    bindings_.swap(other.bindings_);
    snapshotCache_.swap(other.snapshotCache_);
    pointResidency_.swap(other.pointResidency_);
    preparedPoints_.swap(other.preparedPoints_);
}

std::size_t SceneRuntime::bindingCount() const noexcept
{
    return bindings_.size();
}

std::size_t SceneRuntime::pointBindingCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(bindings_, [](const auto &entry) {
            return std::holds_alternative<PointRuntimeBinding>(entry.second);
        }));
}

std::size_t SceneRuntime::rasterBindingCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(bindings_, [](const auto &entry) {
            return std::holds_alternative<RasterRuntimeBinding>(entry.second);
        }));
}

SceneRuntimeSnapshotPtr SceneRuntime::snapshot() const
{
    if (!snapshotCache_) {
        snapshotCache_ = std::shared_ptr<const SceneRuntimeSnapshot>(
            new SceneRuntimeSnapshot(bindings_, pointResidency_));
    }
    return snapshotCache_;
}

std::uint64_t SceneRuntime::decodedByteBudget() const noexcept
{
    return pointResidency_->decodedByteBudget();
}

std::uint64_t SceneRuntime::decodedResidentBytes() const
{
    return pointResidency_->decodedResidentBytes();
}

const HierarchyResidencyCoordinatorPtr &
SceneRuntime::residencyCoordinator() const noexcept
{
    return pointResidency_->coordinator();
}

const PointMemoryBudgetPtr &SceneRuntime::memoryBudget() const noexcept
{
    return pointResidency_->memoryBudget();
}

const DecodedPageCachePtr &SceneRuntime::decodedPageCache() const noexcept
{
    return pointResidency_->cache();
}

const std::shared_ptr<TaskScheduler> &
SceneRuntime::hierarchyScheduler() const noexcept
{
    return pointResidency_->scheduler();
}

SceneRuntimeMetrics SceneRuntime::metrics() const
{
    SceneRuntimeMetrics result;
    bool hasSource = false;
    bool fetchedBytesKnown = true;
    for (const auto &[generation, runtimeBinding] : bindings_) {
        static_cast<void>(generation);
        const PointRuntimeBinding *binding =
            bindingAs<PointRuntimeBinding>(runtimeBinding);
        if (!binding) {
            continue;
        }
        const PointDatasetRuntimePtr &runtime = binding->runtime;
        const RasterPointColorMetrics color =
            runtime->rasterPointColorMetrics();
        result.activeColorTableBytes = saturatingAdd(
            result.activeColorTableBytes, color.activeColorTableBytes);
        result.flatDisplacedColorBytes = saturatingAdd(
            result.flatDisplacedColorBytes, color.flatDisplacedColorBytes);
        result.retainedSourceRootBytes = saturatingAdd(
            result.retainedSourceRootBytes, color.retainedSourceRootBytes);
        result.retainedColoredRootBytes = saturatingAdd(
            result.retainedColoredRootBytes, color.retainedColoredRootBytes);
        const PointCloudStorageMetrics storage = runtime->storageMetrics();
        result.persistentIndexBytes =
            saturatingAdd(result.persistentIndexBytes, storage.persistentBytes);
        if (storage.localPersistent) {
            ++result.localPersistentSources;
            if (storage.reused) {
                ++result.reusedPersistentSources;
            }
        }
        if (!runtime->hierarchical()) {
            result.retainedFlatBytes = saturatingAdd(
                result.retainedFlatBytes, runtime->decodedResidentBytes());
            continue;
        }
        ++result.hierarchicalLayers;
        const PointDatasetRuntimeMetrics scene = runtime->hierarchyMetrics();
        result.source.requests =
            saturatingAdd(result.source.requests, scene.source.requests);
        result.source.completed =
            saturatingAdd(result.source.completed, scene.source.completed);
        result.source.cancelled =
            saturatingAdd(result.source.cancelled, scene.source.cancelled);
        result.source.failed =
            saturatingAdd(result.source.failed, scene.source.failed);
        result.source.estimatedDecodedBytesRequested =
            saturatingAdd(result.source.estimatedDecodedBytesRequested,
                          scene.source.estimatedDecodedBytesRequested);
        result.source.decodedBytesProduced =
            saturatingAdd(result.source.decodedBytesProduced,
                          scene.source.decodedBytesProduced);
        result.source.sourcePointsVisited =
            saturatingAdd(result.source.sourcePointsVisited,
                          scene.source.sourcePointsVisited);
        result.source.decodedPointsProduced =
            saturatingAdd(result.source.decodedPointsProduced,
                          scene.source.decodedPointsProduced);
        result.source.totalQueryNanoseconds =
            saturatingAdd(result.source.totalQueryNanoseconds,
                          scene.source.totalQueryNanoseconds);
        result.source.fetchedBytes = saturatingAdd(result.source.fetchedBytes,
                                                   scene.source.fetchedBytes);
        fetchedBytesKnown = fetchedBytesKnown && scene.source.fetchedBytesKnown;
        hasSource = true;

        result.decodeRequestsQueued = saturatingAdd(result.decodeRequestsQueued,
                                                    scene.decodeRequestsQueued);
        result.decodeRequestsStarted = saturatingAdd(
            result.decodeRequestsStarted, scene.decodeRequestsStarted);
        result.decodeRequestsCompleted = saturatingAdd(
            result.decodeRequestsCompleted, scene.decodeRequestsCompleted);
        result.decodeRequestsCancelled = saturatingAdd(
            result.decodeRequestsCancelled, scene.decodeRequestsCancelled);
        result.decodeRequestsFailed = saturatingAdd(result.decodeRequestsFailed,
                                                    scene.decodeRequestsFailed);
    }
    result.cache = pointResidency_->cache()->metrics();
    result.source.fetchedBytesKnown = hasSource && fetchedBytesKnown;
    result.decodeAdmission =
        pointResidency_->coordinator()->decodeAdmission()->metrics();
    result.memoryBudget = pointResidency_->memoryBudget()->metrics();
    result.scheduler = pointResidency_->scheduler()->metrics();
    return result;
}

void SceneRuntime::syncResidencyBudgets()
{
    rebalanceHierarchyResidency();
}

bool SceneRuntime::contains(const PointCloudSourceId sourceId) const noexcept
{
    return std::ranges::any_of(bindings_, [sourceId](const auto &entry) {
        const PointRuntimeBinding *binding =
            bindingAs<PointRuntimeBinding>(entry.second);
        return binding && binding->descriptor.sourceId == sourceId;
    });
}

bool SceneRuntime::contains(const RasterSourceId sourceId) const noexcept
{
    return std::ranges::any_of(bindings_, [sourceId](const auto &entry) {
        const RasterRuntimeBinding *binding =
            bindingAs<RasterRuntimeBinding>(entry.second);
        return binding && binding->descriptor.sourceId == sourceId;
    });
}

void SceneRuntime::rebalanceHierarchyResidency()
{
    fitHierarchyRoots({}, true);
    pointResidency_->syncCacheBudget();
    for (const auto &[generation, runtimeBinding] : bindings_) {
        static_cast<void>(generation);
        if (const PointRuntimeBinding *binding =
                bindingAs<PointRuntimeBinding>(runtimeBinding)) {
            binding->runtime->syncHierarchyResidencyBudget();
        }
    }
}

void SceneRuntime::fitHierarchyRoots(
    const std::shared_ptr<PointDatasetRuntime> &incoming,
    const bool restoreMinimumBudget)
{
    std::vector<std::shared_ptr<PointDatasetRuntime>> scenes;
    scenes.reserve(pointBindingCount() + (incoming ? 1U : 0U));
    for (const auto &[generation, runtimeBinding] : bindings_) {
        static_cast<void>(generation);
        const PointRuntimeBinding *binding =
            bindingAs<PointRuntimeBinding>(runtimeBinding);
        if (binding && binding->runtime->hierarchical()) {
            scenes.push_back(binding->runtime);
        }
    }
    if (incoming && incoming->hierarchical() &&
        std::ranges::find(scenes, incoming) == scenes.end()) {
        scenes.push_back(incoming);
    }
    if (scenes.empty()) {
        return;
    }

    std::uint64_t minimumBytes = 0;
    std::vector<std::uint64_t> minimumByScene;
    minimumByScene.reserve(scenes.size());
    for (const auto &scene : scenes) {
        const std::uint64_t minimum = std::max(scene->minimumRootPayloadBytes(),
                                               scene->reservedRootBytes());
        minimumByScene.push_back(minimum);
        minimumBytes = saturatingAdd(minimumBytes, minimum);
    }
    std::uint64_t available = pointResidency_->memoryBudget()->availableBytes();
    std::uint64_t reservedRoots = 0;
    for (const auto &scene : scenes) {
        reservedRoots =
            saturatingAdd(reservedRoots, scene->reservedRootBytes());
    }
    available = saturatingAdd(available, reservedRoots);
    if (minimumBytes > available) {
        if (!restoreMinimumBudget) {
            throw std::length_error(
                "the runtime CPU budget cannot retain one hierarchy preview "
                "point per source; increase the CPU cache budget or split "
                "the document");
        }
        const std::uint64_t required =
            saturatingAdd(pointResidency_->memoryBudget()->reservedBytes(),
                          minimumBytes - reservedRoots);
        if (!pointResidency_->memoryBudget()->setByteBudget(required)) {
            throw std::length_error(
                "the point-memory budget cannot retain hierarchy roots");
        }
        available = saturatingAdd(
            pointResidency_->memoryBudget()->availableBytes(), reservedRoots);
    }

    const std::uint64_t discretionary = available - minimumBytes;
    const std::uint64_t count = scenes.size();
    const std::uint64_t common = discretionary / count;
    const std::uint64_t extra = discretionary % count;
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const std::uint64_t allocation =
            minimumByScene[index] + common + (index < extra ? 1U : 0U);
        static_cast<void>(scenes[index]->limitRootPayloadBytes(
            std::max(allocation, scenes[index]->reservedRootBytes())));
    }
}

} // namespace pci
