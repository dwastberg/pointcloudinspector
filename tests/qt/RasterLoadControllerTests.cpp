#include "import/RasterLoadController.h"

#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace {

class StubRasterSource final : public pci::RasterTileSource {
public:
    explicit StubRasterSource(pci::RasterLayerMetadata metadata)
        : metadata_(std::move(metadata))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("the stub source holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::RasterLayerDataPtr stubData(const bool disjoint = false)
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 16;
    metadata.height = 16;
    metadata.geoTransform = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
    metadata.extentDisjointXY = disjoint;
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<StubRasterSource>(std::move(metadata)),
    });
}

// Reports both inspection phases so the controller's state machine can be
// observed, and can be told to fail or to block until released.
class FakeRasterLoader final : public pci::RasterLoader {
public:
    [[nodiscard]] pci::RasterImportPreflight
    inspect(const pci::RasterImportRequest &request) const override
    {
        ++inspectCalls;
        if (request.phase) {
            request.phase(pci::RasterImportPhase::Inspecting);
        }
        while (blocked.load()) {
            if (request.stopToken.stop_requested()) {
                throw pci::RasterImportCancelled("cancelled while blocked");
            }
            QTest::qSleep(1);
        }
        if (request.stopToken.stop_requested()) {
            throw pci::RasterImportCancelled("cancelled");
        }
        if (failuresRemaining > 0) {
            --failuresRemaining;
            throw pci::RasterImportError("stub raster failure");
        }
        if (request.phase) {
            request.phase(pci::RasterImportPhase::SamplingRange);
        }
        return {.data = stubData(disjoint)};
    }

    mutable std::atomic_int inspectCalls{0};
    mutable std::atomic_int failuresRemaining{0};
    std::atomic_bool blocked{false};
    bool disjoint = false;
};

[[nodiscard]] pci::RasterImportRequest request(const char *path = "stub.tif")
{
    pci::RasterImportRequest value;
    value.sourcePath = path;
    return value;
}

TEST_CASE("raster load controller publishes an inspected source",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    pci::RasterLoadController controller(loader, scheduler);

    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);
    const pci::LoadJobId job =
        controller.startImport(request("orthophoto.tif"));
    REQUIRE(loaded.wait(2000));

    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().at(0).value<pci::LoadJobId>() == job);
    // The already-inspected source is handed over rather than reopened on the
    // UI thread.
    CHECK(loaded.front().at(1).value<pci::RasterLayerDataPtr>() != nullptr);
    CHECK(loaded.front().at(2).toBool());

    REQUIRE(controller.jobState(job).has_value());
    CHECK(controller.jobState(job)->phase == pci::RasterLoadJobPhase::Ready);
    CHECK_FALSE(controller.hasActiveJobs());

    const std::vector<pci::LoadJobRow> rows = controller.jobRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().key.kind == pci::LoadJobKind::Raster);
    CHECK(rows.front().title == QStringLiteral("orthophoto.tif"));
    CHECK(rows.front().terminal);
    CHECK(rows.front().capabilities.canDismiss);
    CHECK_FALSE(rows.front().capabilities.canRetry);
}

TEST_CASE("raster load controller hides a disjoint layer on arrival",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    loader->disjoint = true;
    pci::RasterLoadController controller(loader, scheduler);

    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);
    static_cast<void>(controller.startImport(request()));
    REQUIRE(loaded.wait(2000));

    // A raster sharing no XY extent with the scene starts hidden rather than
    // framing the document somewhere the user is not looking.
    CHECK_FALSE(loaded.front().at(2).toBool());
}

TEST_CASE("raster load controller reports the range-sampling phase",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    pci::RasterLoadController controller(loader, scheduler);

    std::vector<pci::RasterLoadJobPhase> observed;
    QObject::connect(&controller,
                     &pci::RasterLoadController::jobStateChanged,
                     &controller,
                     [&controller, &observed](const pci::LoadJobId id) {
                         if (const auto state = controller.jobState(id)) {
                             observed.push_back(state->phase);
                         }
                     });

    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);
    static_cast<void>(controller.startImport(request()));
    REQUIRE(loaded.wait(2000));

    // Range sampling is the only part of inspection whose cost depends on the
    // source rather than its header, so it is reported separately.
    CHECK(std::ranges::find(observed, pci::RasterLoadJobPhase::SamplingRange) !=
          observed.end());
    CHECK(observed.back() == pci::RasterLoadJobPhase::Ready);
}

TEST_CASE("raster load controller fails one source without the others",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    loader->failuresRemaining = 1;
    pci::RasterLoadController controller(loader, scheduler);

    QSignalSpy failed(&controller, &pci::RasterLoadController::failed);
    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);

    const pci::LoadJobId first = controller.startImport(request("broken.tif"));
    const pci::LoadJobId second = controller.startImport(request("good.tif"));
    REQUIRE(failed.wait(2000));
    REQUIRE(QTest::qWaitFor(
        [&loaded] {
            return loaded.size() == 1;
        },
        2000));

    // Each selected path is a separate job, so one failure preserves the rest.
    CHECK(controller.jobState(first)->phase == pci::RasterLoadJobPhase::Failed);
    CHECK(controller.jobState(second)->phase == pci::RasterLoadJobPhase::Ready);
    CHECK(failed.front().at(1).toString().contains(
        QStringLiteral("stub raster failure")));

    const std::vector<pci::LoadJobRow> rows = controller.jobRows();
    REQUIRE(rows.size() == 2);
    CHECK(rows.front().capabilities.canRetry);

    // Retry re-runs the same job identity rather than creating a new row.
    CHECK(controller.retry(first));
    REQUIRE(QTest::qWaitFor(
        [&loaded] {
            return loaded.size() == 2;
        },
        2000));
    CHECK(controller.jobState(first)->phase == pci::RasterLoadJobPhase::Ready);
    CHECK(controller.jobRows().size() == 2);
}

TEST_CASE("raster load controller cancels without emitting a failure",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    loader->blocked = true;
    pci::RasterLoadController controller(loader, scheduler);

    QSignalSpy cancelled(&controller, &pci::RasterLoadController::cancelled);
    QSignalSpy failed(&controller, &pci::RasterLoadController::failed);
    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);

    const pci::LoadJobId job = controller.startImport(request());
    REQUIRE(QTest::qWaitFor(
        [&loader] {
            return loader->inspectCalls.load() > 0;
        },
        2000));

    controller.cancel(job);
    loader->blocked = false;
    REQUIRE(cancelled.wait(2000));

    // Cancellation is not a failure, and it never publishes a layer.
    CHECK(failed.empty());
    CHECK(loaded.empty());
    CHECK(controller.jobState(job)->phase ==
          pci::RasterLoadJobPhase::Cancelled);
    CHECK_FALSE(controller.hasActiveJobs());
}

TEST_CASE("raster load controller dismisses only terminal jobs",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    pci::RasterLoadController controller(loader, scheduler);

    QSignalSpy loaded(&controller, &pci::RasterLoadController::loaded);
    const pci::LoadJobId job = controller.startImport(request());
    REQUIRE(loaded.wait(2000));

    CHECK(controller.dismiss(job));
    CHECK(controller.jobRows().empty());
    CHECK_FALSE(controller.jobState(job).has_value());
    CHECK_FALSE(controller.dismiss(job));
}

TEST_CASE("raster load controller rejects an empty path",
          "[qt][raster][import]")
{
    pci::TaskScheduler scheduler(1, 1024 * 1024);
    auto loader = std::make_shared<FakeRasterLoader>();
    pci::RasterLoadController controller(loader, scheduler);

    CHECK_THROWS_AS(controller.startImport({}), std::invalid_argument);
    CHECK(controller.jobRows().empty());
}

} // namespace
