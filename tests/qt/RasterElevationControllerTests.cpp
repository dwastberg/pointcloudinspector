#include <pci/desktop/operations/RasterElevationController.h>

#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace {

class ElevationSource final : public pci::RasterTileSource {
public:
    ElevationSource()
    {
        metadata_.width = 16;
        metadata_.height = 16;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
        metadata_.bounds = *pci::rasterPixelEdgeBounds(
            metadata_.geoTransform, metadata_.width, metadata_.height);
        metadata_.elevation.available = true;
        metadata_.elevation.band = 1;
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("unused tile read");
    }

    [[nodiscard]] std::uint64_t
    exactElevationScanReservationBytes() const noexcept override
    {
        return 1024;
    }

    [[nodiscard]] pci::RasterElevationRange exactElevationRange(
        const std::stop_token stop,
        pci::RasterElevationProgressCallback progress = {}) const override
    {
        ++calls;
        const int now = ++active;
        int observed = peak.load();
        while (observed < now && !peak.compare_exchange_weak(observed, now)) {
        }
        struct ActiveGuard {
            std::atomic_int *active;
            ~ActiveGuard()
            {
                --*active;
            }
        } guard{&active};
        if (progress) {
            progress({.processedBlocks = 0, .totalBlocks = 2});
        }
        while (blocked.load()) {
            if (stop.stop_requested()) {
                throw pci::RasterReadCancelled{};
            }
            QTest::qSleep(1);
        }
        if (progress) {
            progress({.processedBlocks = 2, .totalBlocks = 2});
        }
        return {.minimum = 10.0, .maximum = 20.0};
    }

    mutable std::atomic_int calls{0};
    mutable std::atomic_int active{0};
    mutable std::atomic_int peak{0};
    std::atomic_bool blocked{true};

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::RasterLayerDataPtr
dataFor(const std::shared_ptr<ElevationSource> &source)
{
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = source,
    });
}

TEST_CASE("DEM exact scans serialize without occupying both import workers",
          "[qt][raster][surface][scheduler]")
{
    pci::TaskScheduler scheduler(2, 1024 * 1024);
    pci::RasterElevationController controller(scheduler);
    auto source = std::make_shared<ElevationSource>();
    QSignalSpy completed(&controller,
                         &pci::RasterElevationController::completed);

    const pci::RasterLayerDataPtr first = dataFor(source);
    const pci::RasterLayerDataPtr second = dataFor(source);
    static_cast<void>(
        controller.start({.layerId = pci::SceneLayerId{1},
                          .sourceId = first->sourceId,
                          .bindingGeneration = pci::BindingGeneration{1}},
                         first->source));
    static_cast<void>(
        controller.start({.layerId = pci::SceneLayerId{2},
                          .sourceId = second->sourceId,
                          .bindingGeneration = pci::BindingGeneration{2}},
                         second->source));
    REQUIRE(QTest::qWaitFor(
        [&source] {
            return source->active.load() == 1;
        },
        2000));
    CHECK(source->calls.load() == 1);
    CHECK(source->peak.load() == 1);

    // The queued second scan is held by the controller, not submitted to the
    // non-preemptive scheduler. An inspection-priority task can therefore use
    // the other worker immediately instead of waiting behind two DEM scans.
    std::atomic_bool inspectionRan{false};
    static_cast<void>(
        scheduler.submit(pci::TaskPriority::Inspection, 1, [&inspectionRan] {
            inspectionRan = true;
        }));
    REQUIRE(QTest::qWaitFor(
        [&inspectionRan] {
            return inspectionRan.load();
        },
        2000));

    source->blocked = false;
    REQUIRE(QTest::qWaitFor(
        [&completed] {
            return completed.size() == 2;
        },
        2000));
    CHECK(source->calls.load() == 2);
    CHECK(source->peak.load() == 1);
    CHECK_FALSE(controller.hasActiveJobs());
}

} // namespace
