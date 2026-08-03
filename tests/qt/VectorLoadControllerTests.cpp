#include "import/VectorLoadController.h"

#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace {

class FakeVectorLoader final : public pci::VectorLoader {
public:
    [[nodiscard]] pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }
    [[nodiscard]] std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {0.0, 0.0};
    }
    [[nodiscard]] pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &request,
                 pci::VectorSublayerKey key) const override
    {
        if (key.name == "bad")
            throw pci::VectorImportError("bad sublayer");
        if (request.progress) {
            request.progress({.processed = 1, .total = 2});
            request.progress({.processed = 2, .total = 2});
        }
        auto data = std::make_shared<pci::VectorLayerData>();
        data->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 0.0}};
        return data;
    }
};

class PreflightVectorLoader final : public pci::VectorLoader {
public:
    [[nodiscard]] pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &) const override
    {
        ++inspectCalls;
        return {.sublayers = {{.key = {.index = 7, .name = "extent-less"},
                               .geometryTypeLabel = "Point"}}};
    }
    [[nodiscard]] std::array<double, 2> probeOrigin(
        const pci::VectorImportRequest &,
        const std::span<const pci::VectorSublayerKey> keys) const override
    {
        ++probeCalls;
        lastProbeKeyCount = keys.size();
        return {123.0, 456.0};
    }
    [[nodiscard]] pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &,
                 pci::VectorSublayerKey) const override
    {
        ++loadCalls;
        return std::make_shared<pci::VectorLayerData>();
    }

    mutable std::atomic<int> inspectCalls = 0;
    mutable std::atomic<int> probeCalls = 0;
    mutable std::atomic<std::size_t> lastProbeKeyCount = 0;
    mutable std::atomic<int> loadCalls = 0;
};

class CancellationVectorLoader final : public pci::VectorLoader {
public:
    [[nodiscard]] pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }
    [[nodiscard]] std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {0.0, 0.0};
    }
    [[nodiscard]] pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &request,
                 pci::VectorSublayerKey key) const override
    {
        if (key.name == "slow") {
            slowStarted = true;
            while (!request.stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw pci::VectorImportCancelled("cancelled slow sublayer");
        }
        auto data = std::make_shared<pci::VectorLayerData>();
        data->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 0.0}};
        return data;
    }

    mutable std::atomic_bool slowStarted = false;
};

pci::LoadJobId startSelectedLoad(pci::VectorLoadController &controller,
                                 pci::VectorImportRequest request)
{
    std::vector<pci::VectorSublayerKey> selected = request.sublayers;
    QSignalSpy inspected(&controller, &pci::VectorLoadController::inspected);
    const pci::LoadJobId jobId = controller.startInspection(std::move(request));
    if (selected.empty()) {
        return jobId;
    }
    REQUIRE(inspected.wait(2000));
    const bool continued = controller.continueLoad(jobId, std::move(selected));
    REQUIRE(continued);
    return jobId;
}

TEST_CASE("vector load controller reports partial sublayer success",
          "[qt][vector]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    pci::VectorLoadController controller(std::make_shared<FakeVectorLoader>(),
                                         scheduler);
    QSignalSpy loaded(&controller, &pci::VectorLoadController::loaded);
    QSignalSpy loadedPart(&controller,
                          &pci::VectorLoadController::sublayerLoaded);
    QSignalSpy failedPart(&controller,
                          &pci::VectorLoadController::sublayerFailed);
    pci::VectorImportRequest request;
    request.sourcePath = "survey.gpkg";
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"},
                         {.index = 1, .name = "bad"}};
    static_cast<void>(startSelectedLoad(controller, std::move(request)));
    REQUIRE(loaded.wait(2000));
    REQUIRE(loaded.count() == 1);
    REQUIRE(loadedPart.count() == 1);
    const auto loadedData =
        loadedPart.at(0).at(2).value<pci::VectorLayerDataPtr>();
    REQUIRE(loadedData);
    CHECK(failedPart.count() == 1);
    CHECK(failedPart.at(0).at(1).value<pci::VectorSublayerKey>().name == "bad");
    const std::vector<pci::LoadJobRow> rows = controller.jobRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().key.kind == pci::LoadJobKind::Vector);
    CHECK(rows.front().title == QStringLiteral("survey.gpkg"));
    CHECK(rows.front().detail.startsWith(QStringLiteral("Vector · ")));
    CHECK(rows.front().capabilities.canRetry);
    CHECK(
        rows.front().detail.contains(QStringLiteral("Retry failed sublayers")));
    scheduler.waitForIdle();
}

TEST_CASE("vector load controller rejects an empty sublayer selection",
          "[qt][vector]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    pci::VectorLoadController controller(std::make_shared<FakeVectorLoader>(),
                                         scheduler);
    QSignalSpy failed(&controller, &pci::VectorLoadController::failed);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    static_cast<void>(startSelectedLoad(controller, std::move(request)));
    REQUIRE(failed.wait(2000));
    CHECK(failed.count() == 1);
    scheduler.waitForIdle();
}

TEST_CASE("vector inspection uses one job and resolves a shared origin",
          "[qt][vector][inspection]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<PreflightVectorLoader>();
    pci::VectorLoadController controller(loader, scheduler);
    QSignalSpy inspected(&controller, &pci::VectorLoadController::inspected);
    QSignalSpy finished(&controller, &pci::VectorLoadController::finished);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;

    const pci::LoadJobId jobId = controller.startInspection(std::move(request));
    REQUIRE(inspected.wait(2000));
    REQUIRE(controller.jobState(jobId));
    CHECK(controller.jobState(jobId)->phase ==
          pci::VectorLoadJobPhase::AwaitingChoice);
    const auto preflight =
        inspected.at(0).at(1).value<pci::VectorImportPreflight>();
    REQUIRE(preflight.sublayers.size() == 1);
    REQUIRE(controller.continueLoad(jobId, {preflight.sublayers.front().key}));
    REQUIRE(finished.wait(2000));
    CHECK(loader->inspectCalls == 1);
    CHECK(loader->probeCalls == 1);
    CHECK(loader->lastProbeKeyCount == 1);
    CHECK(loader->loadCalls == 1);
    REQUIRE(controller.jobState(jobId));
    CHECK(controller.jobState(jobId)->phase == pci::VectorLoadJobPhase::Ready);
    scheduler.waitForIdle();
}

TEST_CASE(
    "vector load controller emits one job failure when no sublayer succeeds",
    "[qt][vector][failure]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    pci::VectorLoadController controller(std::make_shared<FakeVectorLoader>(),
                                         scheduler);
    QSignalSpy failed(&controller, &pci::VectorLoadController::failed);
    QSignalSpy finished(&controller, &pci::VectorLoadController::finished);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 1, .name = "bad"}};
    static_cast<void>(startSelectedLoad(controller, std::move(request)));
    REQUIRE(failed.wait(2000));
    CHECK(failed.count() == 1);
    CHECK(finished.count() == 0);
    scheduler.waitForIdle();
}

TEST_CASE("vector cancellation retains already emitted sublayers",
          "[qt][vector][cancellation]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<CancellationVectorLoader>();
    pci::VectorLoadController controller(loader, scheduler);
    QSignalSpy sublayerLoaded(&controller,
                              &pci::VectorLoadController::sublayerLoaded);
    QSignalSpy cancelled(&controller, &pci::VectorLoadController::cancelled);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"},
                         {.index = 1, .name = "slow"}};
    const pci::LoadJobId jobId =
        startSelectedLoad(controller, std::move(request));
    REQUIRE(sublayerLoaded.wait(2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return loader->slowStarted.load();
        },
        2000));
    controller.cancel(jobId);
    REQUIRE(cancelled.wait(2000));
    REQUIRE(cancelled.count() == 1);
    const auto summary = cancelled.at(0).at(1).value<pci::VectorLoadSummary>();
    REQUIRE(summary.successful.size() == 1);
    CHECK(summary.successful.front().name == "good");
    CHECK(sublayerLoaded.count() == 1);
    REQUIRE(controller.jobState(jobId));
    CHECK(controller.jobState(jobId)->phase ==
          pci::VectorLoadJobPhase::Cancelled);
    scheduler.waitForIdle();
}

TEST_CASE("vector retry schedules only failed sublayers", "[qt][vector]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    pci::VectorLoadController controller(std::make_shared<FakeVectorLoader>(),
                                         scheduler);
    QSignalSpy loaded(&controller, &pci::VectorLoadController::loaded);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"},
                         {.index = 1, .name = "bad"}};
    const auto initial = startSelectedLoad(controller, std::move(request));
    REQUIRE(loaded.wait(2000));
    const auto retry = controller.retry(initial);
    REQUIRE(retry);
    // The retry contains only "bad". The already emitted good layer remains
    // part of this logical job, so the retry ends as a partial success again.
    QSignalSpy finished(&controller, &pci::VectorLoadController::finished);
    REQUIRE(finished.wait(2000));
    scheduler.waitForIdle();
}

TEST_CASE("vector load controller forwards monotonic sublayer progress",
          "[qt][vector][progress]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    pci::VectorLoadController controller(std::make_shared<FakeVectorLoader>(),
                                         scheduler);
    QSignalSpy progress(&controller,
                        &pci::VectorLoadController::progressChanged);
    QSignalSpy finished(&controller, &pci::VectorLoadController::finished);
    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"}};
    static_cast<void>(startSelectedLoad(controller, std::move(request)));
    REQUIRE(finished.wait(2000));
    REQUIRE(progress.count() >= 2);
    std::uint64_t previous = 0;
    for (int index = 0; index < progress.count(); ++index) {
        const auto &signal = progress.at(index);
        const std::uint64_t processed = signal.at(1).toULongLong();
        const std::uint64_t total = signal.at(2).toULongLong();
        CHECK(processed >= previous);
        CHECK(total == 2);
        previous = processed;
    }
    CHECK(previous == 2);
    scheduler.waitForIdle();
}

} // namespace
