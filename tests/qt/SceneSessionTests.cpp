#include "app/SceneSession.h"
#include "support/TestPointColorMaps.h"

#include "import/PointCloudLoadController.h"

#include <catch2/catch_test_macros.hpp>

#include <QTest>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <typename Predicate> bool waitFor(Predicate &&predicate)
{
    return QTest::qWaitFor(std::forward<Predicate>(predicate), 2000);
}

pci::PointCloudScenePtr makeScene(const std::filesystem::path &sourcePath)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = sourcePath;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    metadata.hasColor = true;
    metadata.hasClassification = true;
    auto scene = std::make_shared<pci::PointCloudScene>(std::move(metadata));
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(1);
    block->attributes.resize(1);
    scene->addBlock(std::move(block));
    return scene;
}

class SessionLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic_bool slowStarted = false;
    mutable std::atomic_bool releaseSlow = false;
    mutable std::atomic_bool previewPublished = false;
    mutable std::atomic_bool releaseFailure = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        const std::filesystem::path filename = options.sourcePath.filename();
        if (filename == "slow-first.las") {
            slowStarted = true;
            waitUntil(releaseSlow, context.stopToken);
        }

        pci::PointCloudScenePtr scene = makeScene(options.sourcePath);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }

        if (filename == "cancel-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseSlow, context.stopToken);
        } else if (filename == "fail-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseFailure, context.stopToken);
            throw pci::PointCloudImportError("failure after preview");
        }
        return scene;
    }

private:
    static void waitUntil(const std::atomic_bool &released,
                          const std::stop_token stopToken)
    {
        while (!released.load() && !stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stopToken.stop_requested()) {
            throw pci::PointCloudImportCancelled();
        }
    }
};

class UnavailableVectorLoader final : public pci::VectorLoader {
public:
    pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }

    std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {};
    }

    pci::VectorLayerDataPtr loadSublayer(const pci::VectorImportRequest &,
                                         pci::VectorSublayerKey) const override
    {
        throw std::runtime_error("unused vector loader");
    }
};

class PartialVectorLoader final : public pci::VectorLoader {
public:
    pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }

    std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {};
    }

    pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &,
                 const pci::VectorSublayerKey key) const override
    {
        if (key.name == "bad") {
            throw pci::VectorImportError("bad sublayer");
        }
        auto data = std::make_shared<pci::VectorLayerData>();
        data->sublayerName = key.name;
        data->bounds = {
            .minimum = {0.0, 0.0, 0.0},
            .maximum = {1.0, 1.0, 0.0},
        };
        return data;
    }
};

class UnavailableStatistics final : public pci::PointCloudStatisticsProvider {
public:
    pci::PointCloudStatistics calculate(const pci::PointCloudMetadata &,
                                        std::stop_token,
                                        Progress) const override
    {
        throw std::runtime_error("unused statistics provider");
    }
};

pci::ImportServices
makeServices(const std::shared_ptr<const pci::PointCloudLoader> &loader)
{
    pci::ImportServices services;
    services.scheduler = std::make_unique<pci::TaskScheduler>();
    services.pointCloud = std::make_unique<pci::PointCloudLoadController>(
        loader, *services.scheduler);
    services.vector = std::make_unique<pci::VectorLoadController>(
        std::make_shared<UnavailableVectorLoader>(), *services.scheduler);
    services.statistics = std::make_shared<UnavailableStatistics>();
    return services;
}

pci::ImportServices
makeServices(const std::shared_ptr<const pci::PointCloudLoader> &pointLoader,
             const std::shared_ptr<const pci::VectorLoader> &vectorLoader)
{
    pci::ImportServices services = makeServices(pointLoader);
    services.vector = std::make_unique<pci::VectorLoadController>(
        vectorLoader, *services.scheduler);
    return services;
}

void finishVisibleLoads(pci::SceneSession &session)
{
    REQUIRE(waitFor([&session] {
        return !session.document()->layers().empty();
    }));
    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return !states.empty() &&
               std::ranges::all_of(states, [](const auto &state) {
                   return state.phase == pci::PointCloudLoadJobPhase::Ready;
               });
    }));
    for (const pci::PointCloudLayer &layer : session.document()->layers()) {
        session.onRenderLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::DisplayReady,
        });
    }
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
}

} // namespace

TEST_CASE("scene session retires replace and add loads at display readiness",
          "[scene-session][loading]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());

    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    CHECK(session.document()->layerCount() == 1);

    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    finishVisibleLoads(session);
    CHECK(session.document()->layerCount() == 2);
}

TEST_CASE("scene session orders out-of-order batch previews",
          "[scene-session][batch]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);

    session.loadPointClouds({"slow-first.las", "fast-second.las"},
                            pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->slowStarted.load();
    }));
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 1;
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("fast-second.las"));

    loader->releaseSlow = true;
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    const auto layers = session.document()->layers();
    CHECK(layers[0].scene->metadata().sourcePath ==
          std::filesystem::path("slow-first.las"));
    CHECK(layers[1].scene->metadata().sourcePath ==
          std::filesystem::path("fast-second.las"));
    finishVisibleLoads(session);
}

TEST_CASE("scene session rolls back cancellation and failure after preview",
          "[scene-session][rollback]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    REQUIRE(waitFor([&session] {
        return session.document()
                   ->layers()
                   .front()
                   .scene->metadata()
                   .sourcePath ==
               std::filesystem::path("cancel-after-preview.las");
    }));
    session.cancelAll();
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("original.las"));

    loader->previewPublished = false;
    session.loadPointCloud("fail-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    loader->releaseFailure = true;
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("original.las"));
}

TEST_CASE("scene session owns document commands", "[scene-session][commands]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());
    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    finishVisibleLoads(session);

    const auto layers = session.document()->layers();
    session.setLayerVisible(layers.front().id, false);
    CHECK_FALSE(session.document()->layer(layers.front().id)->visible);
    session.isolateLayer(layers.front().id);
    CHECK(session.document()->layer(layers.front().id)->visible);
    CHECK_FALSE(session.document()->layer(layers.back().id)->visible);
    session.showAllLayers();
    CHECK(session.document()->layer(layers.back().id)->visible);

    const pci::PointColorMode mode{
        .source = pci::PointColorSource::X,
        .colorMap = pci::PointColorMap::Turbo,
    };
    session.setAllLayerColors(mode);
    CHECK(session.document()->layer(layers.front().id)->colorMode == mode);
    CHECK(session.document()->layer(layers.back().id)->colorMode == mode);

    pci::PointClassificationFilter filter =
        pci::PointClassificationFilter::noneVisible();
    filter.setVisible(2, true);
    session.setLayerClassificationFilter(layers.front().id, filter, false);
    CHECK(session.document()->layer(layers.front().id)->classificationFilter ==
          filter);
    session.removeLayer(layers.back().id);
    CHECK(session.document()->layerCount() == 1);
}

TEST_CASE("scene session updates future-load and decoded-memory budgets",
          "[scene-session][settings]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(
        makeServices(loader), 100, std::uint64_t{1024} * 1024);

    session.setMaximumLoadPoints(250);
    CHECK(session.maximumLoadPoints() == 250);
    CHECK(session.setDecodedByteBudget(std::uint64_t{2} * 1024 * 1024,
                                       std::nullopt));
    CHECK(session.decodedByteBudget() == std::uint64_t{2} * 1024 * 1024);
    CHECK_FALSE(session.automaticMemoryBudgetEnabled());
    CHECK(session.document()->decodedByteBudget() ==
          std::uint64_t{2} * 1024 * 1024);
}

TEST_CASE("scene session publishes unified rows for partial vector success",
          "[scene-session][tasks][vector]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto vectorLoader = std::make_shared<PartialVectorLoader>();
    pci::SceneSession session(
        makeServices(pointLoader, vectorLoader), 100, 1024 * 1024);
    pci::LoadJobRows latestRows;
    QObject::connect(&session,
                     &pci::SceneSession::taskRowsChanged,
                     [&latestRows](pci::LoadJobRows rows) {
                         latestRows = std::move(rows);
                     });

    pci::VectorImportRequest request;
    request.sourcePath = "survey.gpkg";
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"},
                         {.index = 1, .name = "bad"}};
    static_cast<void>(session.loadVectorLayers(std::move(request)));

    REQUIRE(waitFor([&session] {
        return session.document()->vectorLayerCount() == 1;
    }));
    REQUIRE(waitFor([&latestRows] {
        return latestRows.size() == 1 && latestRows.front().terminal &&
               latestRows.front().capabilities.canRetry;
    }));
    CHECK(latestRows.front().key.kind == pci::LoadJobKind::Vector);
    CHECK(latestRows.front().capabilities.canRetry);
    CHECK(latestRows.front().detail.contains(
        QStringLiteral("Retry failed sublayers")));
}
