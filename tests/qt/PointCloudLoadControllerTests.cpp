#include "support/DeterministicCompletionExecutor.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/operations/OperationTarget.h>

#include <pci/adapters/storage/SecureStorage.h>
#include <pci/desktop/operations/PointCloudLoadController.h>
#include <pci/document/SceneDocument.h>

#include <pci/operations/PointDatasetInstallation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QVariant>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace pci {
class PointCloudLoadControllerTestAccess {
public:
    static void executor(PointCloudLoadController &controller,
                         const std::shared_ptr<CompletionExecutor> &executor)
    {
        controller.callbackTarget_ =
            std::make_shared<OperationTarget<PointImportOperation>>(&controller,
                                                                    executor);
    }
};
} // namespace pci

namespace {

static_assert(!std::is_convertible_v<pci::LoadJobId, pci::TaskId>);
static_assert(!std::is_convertible_v<pci::LoadJobId, pci::PointCloudSourceId>);
static_assert(!std::is_convertible_v<pci::LoadJobId, pci::SceneLayerId>);
static_assert(!std::is_convertible_v<pci::TaskId, pci::PointCloudSourceId>);

TEST_CASE("domain identifiers remain distinct, hashable Qt value types",
          "[qt][async][strong-id]")
{
    const pci::LoadJobId first{17};
    const pci::LoadJobId same{17};
    const pci::LoadJobId second{23};
    const std::unordered_set<pci::LoadJobId> ids{first, same, second};
    CHECK(ids.size() == 2);

    const QVariant value = QVariant::fromValue(first);
    REQUIRE(value.canConvert<pci::LoadJobId>());
    CHECK(value.value<pci::LoadJobId>() == first);
}

TEST_CASE("shared load-job mechanics allocate and project explicit states",
          "[qt][async][jobs]")
{
    pci::LoadJobIdSequence sequence;
    CHECK(sequence.next("exhausted") == pci::LoadJobId{1});
    CHECK(sequence.next("exhausted") == pci::LoadJobId{2});

    pci::LoadJobIdSequence exhausted{std::numeric_limits<std::uint64_t>::max()};
    CHECK_THROWS_AS(exhausted.next("job ids exhausted"), std::overflow_error);

    CHECK(pci::activeLoadJobCapabilities(true).canCancel);
    CHECK(pci::activeLoadJobCapabilities(true).canPrioritize);
    CHECK_FALSE(pci::activeLoadJobCapabilities(true).canDismiss);
    CHECK(pci::terminalLoadJobCapabilities(true).canRetry);
    CHECK(pci::terminalLoadJobCapabilities(true).canDismiss);
    CHECK_FALSE(pci::terminalLoadJobCapabilities(true).canCancel);
}

std::shared_ptr<pci::PointDatasetPreparation>
makePreparation(const std::uint64_t count)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = count;
    auto scene = std::make_shared<pci::PointDatasetPreparation>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(static_cast<std::size_t>(count));
    block->attributes.resize(static_cast<std::size_t>(count));
    scene->addBlock(std::move(block));
    return scene;
}

class FakeLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic<QThread *> workerThread = nullptr;
    mutable std::atomic<bool> slowStarted = false;

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        workerThread = QThread::currentThread();
        if (options.sourcePath == "failure") {
            throw pci::PointCloudImportError("fixture failure");
        }
        if (options.sourcePath == "slow") {
            slowStarted = true;
            while (!context.stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw pci::PointCloudImportCancelled();
        }

        auto scene = makePreparation(2);
        scene->publish(context);
        if (context.progress) {
            context.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 0,
                .total = 2,
            });
            context.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 2,
                .total = 2,
            });
        }
        return scene->finish();
    }
};

class BatchLoader final : public pci::PointCloudLoader {
public:
    explicit BatchLoader(const int expectedInspections)
        : expectedInspections_(expectedInspections)
    {
    }

    pci::PointCloudImportPreflight
    inspect(const pci::PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token) const override
    {
        ++inspections;
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = options.maximumPoints;
        return {
            .metadata = std::move(metadata),
            .desiredRetainedPoints = options.maximumPoints,
            .estimatedResidentBytes =
                pci::estimatedFlatResidentBytes(options.maximumPoints),
            .estimatedActiveBytes = 10,
            .hierarchical = false,
        };
    }

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &resources,
         const pci::PointCloudImportPreflight &preflight,
         const pci::PointCloudLoadContext &context) const override
    {
        if (inspections.load() != expectedInspections_) {
            loadedBeforePreflight = true;
        }
        const std::uint64_t retained = preflight.retainedPointLimit == 0
                                           ? options.maximumPoints
                                           : preflight.retainedPointLimit;
        {
            const std::scoped_lock lock(mutex);
            retainedLimits.push_back(retained);
            spatialPreview.push_back(preflight.spatialPreview);
        }
        pci::PointCloudMetadata metadata = preflight.metadata;
        auto scene = std::make_shared<pci::PointDatasetPreparation>(metadata);
        if (resources.flatReservation) {
            scene->setResidentMemoryReservation(resources.flatReservation);
        }
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(static_cast<std::size_t>(retained));
        block->attributes.resize(static_cast<std::size_t>(retained));
        scene->addBlock(std::move(block));
        scene->publish(context);
        return scene->finish();
    }

    mutable std::atomic<int> inspections = 0;
    mutable std::atomic<bool> loadedBeforePreflight = false;
    mutable std::mutex mutex;
    mutable std::vector<std::uint64_t> retainedLimits;
    mutable std::vector<bool> spatialPreview;

private:
    int expectedInspections_ = 0;
};

class LocalPagingBatchLoader final : public pci::PointCloudLoader {
public:
    explicit LocalPagingBatchLoader(const int expectedInspections)
        : expectedInspections_(expectedInspections)
    {
    }

    pci::PointCloudImportPreflight
    inspect(const pci::PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token) const override
    {
        ++inspections;
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 7'000'000;
        return {
            .metadata = std::move(metadata),
            .desiredRetainedPoints = 7'000'000,
            .estimatedResidentBytes = rootBytes,
            .estimatedActiveBytes = 10,
            .hierarchical = true,
            .localPaging = true,
        };
    }

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &,
         const pci::PointCloudLoadResources &resources,
         const pci::PointCloudImportPreflight &preflight,
         const pci::PointCloudLoadContext &) const override
    {
        if (inspections.load() != expectedInspections_) {
            loadedBeforePreflight = true;
        }
        if (resources.flatReservation || preflight.retainedPointLimit != 0 ||
            preflight.spatialPreview) {
            receivedFlatAdmission = true;
        }
        return makePreparation(1)->finish();
    }

    static constexpr std::uint64_t rootBytes = 1024;
    mutable std::atomic<int> inspections = 0;
    mutable std::atomic<bool> loadedBeforePreflight = false;
    mutable std::atomic<bool> receivedFlatAdmission = false;

private:
    int expectedInspections_ = 0;
};

class BlockingBatchLoader final : public pci::PointCloudLoader {
public:
    pci::PointCloudImportPreflight
    inspect(const pci::PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token) const override
    {
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 1;
        return {
            .metadata = std::move(metadata),
            .desiredRetainedPoints = 1,
            .estimatedResidentBytes = pci::estimatedFlatResidentBytes(1),
            .estimatedActiveBytes = 100,
            .hierarchical = false,
        };
    }

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        ++started;
        while (!release.load() && !context.stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (context.stopToken.stop_requested()) {
            throw pci::PointCloudImportCancelled();
        }
        return makePreparation(1)->finish();
    }

    mutable std::atomic<int> started = 0;
    mutable std::atomic<bool> release = false;
};

class PartiallyFailingInspectionLoader final : public pci::PointCloudLoader {
public:
    pci::PointCloudImportPreflight
    inspect(const pci::PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token) const override
    {
        ++inspections;
        if (options.sourcePath == "bad.las") {
            throw pci::PointCloudImportError("bad preflight");
        }
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 1;
        return {
            .metadata = std::move(metadata),
            .desiredRetainedPoints = 1,
            .estimatedResidentBytes = pci::estimatedFlatResidentBytes(1),
            .estimatedActiveBytes = 1,
            .hierarchical = false,
        };
    }

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &) const override
    {
        if (inspections.load() != 3) {
            loadedBeforePreflight = true;
        }
        ++loads;
        return makePreparation(1)->finish();
    }

    mutable std::atomic<int> inspections = 0;
    mutable std::atomic<int> loads = 0;
    mutable std::atomic<bool> loadedBeforePreflight = false;
};

TEST_CASE("point load options are independent value configuration",
          "[unit][async][options]")
{
    const auto cache = pci::LocalPageCacheContext::createTemporary(
        std::filesystem::temp_directory_path());
    const pci::PointCloudLoadOptions first{
        .sourcePath = "survey.laz",
        .maximumPoints = 42,
        .localPaging =
            {
                .pointThreshold = 7,
                .cache = cache,
                .pagePoints = 8,
            },
    };
    pci::PointCloudLoadOptions second = first;
    CHECK(second == first);

    pci::PointCloudLoadRequest request{
        .options = second,
        .resources =
            {
                .decodedByteBudget = 1024,
                .memoryBudget = std::make_shared<pci::PointMemoryBudget>(1024),
            },
    };
    CHECK(request.options == first);
    request.resources.decodedByteBudget = 2048;
    CHECK(request.options == first);
}

TEST_CASE("load controller runs loaders off-thread and signals its owner",
          "[qt][async]")
{
    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy sceneReady(&controller,
                          &pci::PointCloudLoadController::dataReady);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    QSignalSpy progress(&controller,
                        &pci::PointCloudLoadController::progressChanged);
    REQUIRE(sceneReady.isValid());
    REQUIRE(loaded.isValid());
    REQUIRE(progress.isValid());

    QThread *signalThread = nullptr;
    QObject::connect(&controller,
                     &pci::PointCloudLoadController::loaded,
                     &controller,
                     [&signalThread] {
                         signalThread = QThread::currentThread();
                     });

    controller.load({.sourcePath = "success"});

    REQUIRE(loaded.wait(2000));
    REQUIRE(loaded.count() == 1);
    CHECK(sceneReady.count() == 1);
    REQUIRE(progress.count() >= 1);
    REQUIRE(progress.count() <= 2);
    // Every signal carries the originating job id as its first argument.
    CHECK(qvariant_cast<pci::PointCloudImportStage>(progress.at(0).at(1)) ==
          pci::PointCloudImportStage::Reading);
    CHECK(progress.last().at(2).toULongLong() == 2);
    CHECK(progress.last().at(3).toULongLong() == 2);
    CHECK(loader->workerThread.load() != QThread::currentThread());
    CHECK(signalThread == QThread::currentThread());

    const auto scene =
        qvariant_cast<pci::PreparedPointDatasetPtr>(loaded.at(0).at(1));
    REQUIRE(scene != nullptr);
    CHECK(scene->pointCount() == 2);
    const auto event =
        qvariant_cast<pci::PointDatasetEvent>(sceneReady.at(0).at(1));
    const auto earlyScene = std::get<pci::PreparedPointDatasetPtr>(event.data);
    CHECK(earlyScene->descriptor.sourceId == scene->descriptor.sourceId);
    CHECK(earlyScene->blocks == scene->blocks);
}

TEST_CASE("load controller reports loader failures", "[qt][async]")
{
    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
    REQUIRE(failed.isValid());

    controller.load({.sourcePath = "failure"});

    REQUIRE(failed.wait(2000));
    REQUIRE(failed.count() == 1);
    CHECK(failed.at(0).at(1).toString().contains("fixture failure"));
}

TEST_CASE("load controller runs multiple jobs concurrently", "[qt][async]")
{
    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    QSignalSpy cancelled(&controller,
                         &pci::PointCloudLoadController::cancelled);
    REQUIRE(loaded.isValid());
    REQUIRE(cancelled.isValid());

    // Starting a second load no longer cancels the first: both run to
    // completion and each reports under its own job id.
    const pci::LoadJobId first = controller.load({.sourcePath = "one"});
    const pci::LoadJobId second = controller.load({.sourcePath = "two"});
    CHECK(first != second);

    REQUIRE(QTest::qWaitFor(
        [&] {
            return loaded.count() == 2;
        },
        2000));
    CHECK(cancelled.count() == 0);
    CHECK(loaded.at(0).at(0).value<pci::LoadJobId>() !=
          loaded.at(1).at(0).value<pci::LoadJobId>());
}

TEST_CASE("explicit cancellation emits cancelled", "[qt][async]")
{
    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy cancelled(&controller,
                         &pci::PointCloudLoadController::cancelled);
    REQUIRE(cancelled.isValid());

    controller.load({.sourcePath = "slow"});
    REQUIRE(QTest::qWaitFor(
        [&] {
            return loader->slowStarted.load();
        },
        2000));
    controller.cancel();

    REQUIRE(cancelled.wait(2000));
    CHECK(cancelled.count() == 1);
}

TEST_CASE("batch preflight precedes heavy work and applies fair memory caps",
          "[qt][async][batch][residency]")
{
    constexpr std::uint64_t desiredPoints = 1000;
    for (const std::size_t sourceCount :
         std::array<std::size_t, 4>{1, 7, 25, 100}) {
        const std::uint64_t budgetBytes =
            pci::estimatedFlatResidentBytes(120) * sourceCount;
        auto memory = std::make_shared<pci::PointMemoryBudget>(budgetBytes);
        auto loader =
            std::make_shared<BatchLoader>(static_cast<int>(sourceCount));
        pci::TaskScheduler scheduler(2, 100);
        pci::PointCloudLoadController controller(loader, scheduler);
        QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
        REQUIRE(loaded.isValid());

        std::vector<pci::PointCloudLoadRequest> requests;
        requests.reserve(sourceCount);
        for (std::size_t index = 0; index < sourceCount; ++index) {
            requests.push_back({
                .options =
                    {
                        .sourcePath = std::to_string(index) + ".las",
                        .maximumPoints = desiredPoints,
                    },
                .resources =
                    {
                        .memoryBudget = memory,
                    },
            });
        }
        const auto ids = controller.loadBatch(std::move(requests));
        CHECK(ids.size() == sourceCount);
        REQUIRE(QTest::qWaitFor(
            [&] {
                return loaded.count() == static_cast<qsizetype>(sourceCount);
            },
            5000));

        CHECK_FALSE(loader->loadedBeforePreflight.load());
        REQUIRE(loader->retainedLimits.size() == sourceCount);
        const std::uint64_t firstLimit = loader->retainedLimits.front();
        CHECK(firstLimit > 0);
        CHECK(firstLimit < desiredPoints);
        for (const std::uint64_t limit : loader->retainedLimits) {
            CHECK(limit == firstLimit);
        }
        for (const bool spatial : loader->spatialPreview) {
            CHECK(spatial);
        }
        CHECK(memory->reservedBytes() <= memory->byteBudget());
        const pci::PointCloudLoadControllerMetrics metrics =
            controller.metrics();
        CHECK(metrics.preflightCompleted == sourceCount);
        CHECK(metrics.safetySampledSources == sourceCount);
        CHECK(metrics.scheduler.peakActive <= 2);
        CHECK(metrics.scheduler.peakActiveEstimatedBytes <= 100);
    }
}

TEST_CASE("local page batches bypass retained-flat sampling and reservations",
          "[qt][async][batch][residency][local-pages]")
{
    constexpr std::size_t sourceCount = 25;
    auto memory = std::make_shared<pci::PointMemoryBudget>(
        (sizeof(pci::GpuPoint) + sizeof(pci::PointAttributes)) * sourceCount);
    auto loader =
        std::make_shared<LocalPagingBatchLoader>(static_cast<int>(sourceCount));
    pci::TaskScheduler scheduler(2, 100);
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    REQUIRE(loaded.isValid());

    std::vector<pci::PointCloudLoadRequest> requests;
    requests.reserve(sourceCount);
    for (std::size_t index = 0; index < sourceCount; ++index) {
        requests.push_back({
            .options =
                {
                    .sourcePath = std::to_string(index) + ".laz",
                    .maximumPoints = 10'000'000,
                },
            .resources =
                {
                    .memoryBudget = memory,
                },
        });
    }
    const auto ids = controller.loadBatch(std::move(requests));
    REQUIRE(ids.size() == sourceCount);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return loaded.count() == static_cast<qsizetype>(sourceCount);
        },
        5000));

    CHECK_FALSE(loader->loadedBeforePreflight.load());
    CHECK_FALSE(loader->receivedFlatAdmission.load());
    CHECK(memory->reservedBytes() == 0);
    const pci::PointCloudLoadControllerMetrics metrics = controller.metrics();
    CHECK(metrics.preflightCompleted == sourceCount);
    CHECK(metrics.safetySampledSources == 0);
    CHECK(metrics.admittedFlatReservationBytes == 0);
    CHECK(metrics.scheduler.peakActive <= 2);
}

TEST_CASE("queued batch cancellation does not wait for an active reader",
          "[qt][async][batch][cancellation]")
{
    auto loader = std::make_shared<BlockingBatchLoader>();
    pci::TaskScheduler scheduler(1, 100);
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy cancelled(&controller,
                         &pci::PointCloudLoadController::cancelled);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    REQUIRE(cancelled.isValid());
    REQUIRE(loaded.isValid());

    const auto ids = controller.loadBatch({
        {.options = {.sourcePath = "active", .maximumPoints = 1}},
        {.options = {.sourcePath = "queued", .maximumPoints = 1}},
    });
    REQUIRE(ids.size() == 2);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return loader->started.load() == 1;
        },
        2000));
    controller.cancel(ids[1]);
    REQUIRE(cancelled.wait(1000));
    CHECK(cancelled.at(0).at(0).value<pci::LoadJobId>() == ids[1]);
    CHECK(loader->started.load() == 1);

    loader->release = true;
    REQUIRE(loaded.wait(2000));
    CHECK(loaded.at(0).at(0).value<pci::LoadJobId>() == ids[0]);
}

TEST_CASE("one preflight failure does not prevent other batch sources",
          "[qt][async][batch]")
{
    auto loader = std::make_shared<PartiallyFailingInspectionLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    REQUIRE(failed.isValid());
    REQUIRE(loaded.isValid());

    static_cast<void>(controller.loadBatch({
        {.options = {.sourcePath = "first.las", .maximumPoints = 1}},
        {.options = {.sourcePath = "bad.las", .maximumPoints = 1}},
        {.options = {.sourcePath = "last.las", .maximumPoints = 1}},
    }));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return failed.count() == 1 && loaded.count() == 2;
        },
        2000));
    CHECK(failed.at(0).at(1).toString().contains("bad preflight"));
    CHECK(loader->loads.load() == 2);
    CHECK_FALSE(loader->loadedBeforePreflight.load());
}

TEST_CASE("batch admission reports when minimum previews cannot fit",
          "[qt][async][batch][residency]")
{
    auto loader = std::make_shared<BatchLoader>(2);
    auto memory = std::make_shared<pci::PointMemoryBudget>(
        pci::estimatedFlatResidentBytes(1));
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    REQUIRE(failed.isValid());
    REQUIRE(loaded.isValid());

    static_cast<void>(controller.loadBatch({
        {.options = {.sourcePath = "first.las", .maximumPoints = 10},
         .resources = {.memoryBudget = memory}},
        {.options = {.sourcePath = "second.las", .maximumPoints = 10},
         .resources = {.memoryBudget = memory}},
    }));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return failed.count() == 2;
        },
        2000));
    CHECK(loaded.count() == 0);
    CHECK(failed.at(0).at(1).toString().contains("minimum spatial preview"));
    CHECK(memory->reservedBytes() == 0);
}

TEST_CASE("load job states retain recovery details and weighted progress",
          "[qt][async][progress][recovery]")
{
    std::vector<pci::PointCloudLoadJobState> states{
        {.jobId = pci::LoadJobId{1},
         .phase = pci::PointCloudLoadJobPhase::Reading,
         .processed = 50,
         .total = 100},
        {.jobId = pci::LoadJobId{2},
         .phase = pci::PointCloudLoadJobPhase::Indexing,
         .processed = 25,
         .total = 100,
         .localPaging = true},
        {.jobId = pci::LoadJobId{3},
         .phase = pci::PointCloudLoadJobPhase::Ready},
    };
    CHECK(states[0].weightedCompletion() == Catch::Approx(0.39));
    CHECK(states[1].weightedCompletion() == Catch::Approx(0.3125));
    const pci::PointCloudBatchProgress aggregate =
        pci::weightedBatchProgress(states);
    CHECK(aggregate.sourceCount == 3);
    CHECK(aggregate.completedSources == 1);
    CHECK(aggregate.completion == Catch::Approx((0.39 + 0.3125 + 1.0) / 3.0));

    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
    const pci::LoadJobId jobId = controller.load({.sourcePath = "failure"});
    REQUIRE(failed.wait(2000));
    const auto failedState = controller.jobState(jobId);
    REQUIRE(failedState);
    CHECK(failedState->phase == pci::PointCloudLoadJobPhase::Failed);
    CHECK(failedState->canRetry);
    CHECK_FALSE(failedState->canCancel);
    CHECK(failedState->detail.contains("fixture failure"));
    CHECK(controller.jobStates().size() == 1);
    CHECK(controller.dismiss(jobId));
    CHECK_FALSE(controller.jobState(jobId));
}

class StreamingLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic<int> fullQueues = 0;
    mutable std::atomic<int> stopped = 0;

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        auto forwarded = context;
        if (options.sourcePath == "stale" || options.sourcePath == "sequence") {
            forwarded.dataReady = [&](pci::PointDatasetEvent event) {
                if (options.sourcePath == "stale") {
                    ++event.token.attempt;
                } else {
                    ++event.sequence;
                }
                context.dataReady(std::move(event));
            };
        }
        pci::PointDatasetPreparation dataset({});
        dataset.publish(forwarded);
        if (options.sourcePath == "missing") {
            auto result =
                std::make_shared<pci::PreparedPointDataset>(*dataset.finish());
            ++result->eventCount;
            return result;
        }
        try {
            for (std::size_t i = 0; i < 160; ++i) {
                if (i == 63) {
                    ++fullQueues;
                }
                auto block = std::make_shared<pci::PointBlock>();
                block->points.resize(1);
                dataset.addBlock(std::move(block));
            }
            return dataset.finish();
        } catch (const pci::PointCloudImportCancelled &) {
            ++stopped;
            throw;
        }
    }
};

TEST_CASE("controller wakes drain at most eight events fairly and shutdown "
          "releases full queues",
          "[qt][async][point-import][queue][shutdown]")
{
    auto loader = std::make_shared<StreamingLoader>();
    auto executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    pci::TaskScheduler scheduler(2);
    auto controller =
        std::make_unique<pci::PointCloudLoadController>(loader, scheduler);
    pci::PointCloudLoadControllerTestAccess::executor(*controller, executor);
    QSignalSpy events(controller.get(),
                      &pci::PointCloudLoadController::dataReady);
    QSignalSpy loaded(controller.get(), &pci::PointCloudLoadController::loaded);
    const auto ids = controller->loadBatch({
        {.options = {.sourcePath = "first", .maximumPoints = 160}},
        {.options = {.sourcePath = "second", .maximumPoints = 160}},
    });
    scheduler
        .waitForIdle(); // Inspections only; loading starts on owner delivery.
    REQUIRE(executor->runNext());
    REQUIRE(QTest::qWaitFor(
        [&] {
            return loader->fullQueues.load() == 2;
        },
        2000));
    // Recovery ticks must not steal work from an accepted executor wake.
    QTest::qWait(25);
    REQUIRE(events.isEmpty());
    CHECK(executor->pendingWakeCount() == 1);
    REQUIRE(executor->runNext());
    REQUIRE(events.count() == 8);
    std::array<int, 2> counts{};
    for (const auto &delivery : events) {
        const auto id = delivery.front().value<pci::LoadJobId>();
        ++counts[id == ids.front() ? 0 : 1];
    }
    CHECK(counts[0] == 4);
    CHECK(counts[1] == 4);
    // A producer may claim the next wake while its executor post is in flight.
    REQUIRE(QTest::qWaitFor(
        [&] {
            return executor->pendingWakeCount() == 1;
        },
        2000));
    CHECK(events.count() == 8);
    controller.reset();
    scheduler.waitForIdle(); // No owner drain is required to unblock producers.
    CHECK(loader->stopped == 2);
    while (executor->runNext()) {
    }
    CHECK(events.count() == 8);
    CHECK(loaded.empty());
}

TEST_CASE("controller rejects stale identities sequences and incomplete "
          "terminal streams",
          "[qt][async][point-import][sequence]")
{
    for (const auto path : {"stale", "sequence", "missing"}) {
        CAPTURE(path);
        auto loader = std::make_shared<StreamingLoader>();
        pci::TaskScheduler scheduler;
        pci::PointCloudLoadController controller(loader, scheduler);
        QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
        QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
        const auto id =
            controller.load({.sourcePath = path, .maximumPoints = 160});
        REQUIRE(failed.wait(2000));
        CHECK(failed.count() == 1);
        CHECK(loaded.empty());
        REQUIRE(controller.jobState(id));
        CHECK(controller.jobState(id)->phase ==
              pci::PointCloudLoadJobPhase::Failed);
    }
}

TEST_CASE("owner installation rejection never publishes Ready",
          "[qt][async][point-import][transaction]")
{
    auto loader = std::make_shared<FakeLoader>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    QObject::connect(&controller,
                     &pci::PointCloudLoadController::prepared,
                     &controller,
                     [](pci::LoadJobId, pci::PreparedPointDatasetPtr) {
                         throw std::bad_alloc();
                     });
    QSignalSpy failed(&controller, &pci::PointCloudLoadController::failed);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    const auto id = controller.load({.sourcePath = "success"});
    REQUIRE(failed.wait(2000));
    CHECK(loaded.empty());
    CHECK(controller.jobState(id)->phase ==
          pci::PointCloudLoadJobPhase::Failed);
}

} // namespace

TEST_CASE("a rejected ingestion wake still reaches its terminal state",
          "[qt][async][point-import][terminal]")
{
    auto loader = std::make_shared<FakeLoader>();
    auto executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(loader, scheduler);
    pci::PointCloudLoadControllerTestAccess::executor(controller, executor);
    QSignalSpy loaded(&controller, &pci::PointCloudLoadController::loaded);
    const auto id = controller.load({.sourcePath = "success"});
    scheduler.waitForIdle();
    executor->rejectWakeRequests();
    REQUIRE(executor->runNext()); // Starts the worker; all ingestion wakes are
                                  // rejected.
    REQUIRE(loaded.wait(2000));
    CHECK(loaded.count() == 1);
    CHECK(controller.jobState(id)->phase == pci::PointCloudLoadJobPhase::Ready);
}
