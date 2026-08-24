#include "app/SceneLayerListModel.h"
#include "app/SceneLayersDock.h"
#include "app/TaskDock.h"
#include "app/TaskListModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractItemModelTester>
#include <QListView>
#include <QSignalSpy>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

pci::PointCloudLayer pointLayer(const std::uint64_t id,
                                const char *path,
                                const std::uint64_t count = 10)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = path;
    metadata.sourcePointCount = count;
    return {.id = pci::SceneLayerId{id},
            .scene = std::make_shared<pci::PointCloudScene>(metadata)};
}

pci::SceneDocumentSnapshotPtr
snapshot(std::initializer_list<pci::PointCloudLayer> layers)
{
    auto result = std::make_shared<pci::SceneDocumentSnapshot>();
    for (const auto &layer : layers) {
        result->layers.push_back({
            .id = layer.id,
            .visible = layer.visible,
            .payload =
                pci::PointCloudLayerState{
                    .scene = layer.scene,
                    .colorMode = layer.colorMode,
                    .classificationFilter = layer.classificationFilter,
                    .rasterColors = layer.rasterColors,
                    .colorGeneration = layer.colorGeneration,
                },
        });
    }
    return result;
}

class ListModelRasterSource final : public pci::RasterTileSource {
public:
    explicit ListModelRasterSource(pci::RasterLayerMetadata metadata)
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
        throw pci::RasterReadError("the list fixture holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

pci::RasterLayerDataPtr rasterData(const char *path,
                                   const std::uint32_t width = 1024,
                                   const std::uint32_t height = 768)
{
    pci::RasterLayerMetadata metadata;
    metadata.sourcePath = path;
    metadata.width = width;
    metadata.height = height;
    metadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<ListModelRasterSource>(std::move(metadata)),
    });
}

pci::SceneDocumentSnapshotPtr
rasterSnapshot(std::initializer_list<pci::RasterLayer> layers)
{
    auto result = std::make_shared<pci::SceneDocumentSnapshot>();
    for (const auto &layer : layers) {
        result->layers.push_back({
            .id = layer.id,
            .visible = layer.visible,
            .payload =
                pci::RasterLayerState{.data = layer.data, .style = layer.style},
        });
    }
    return result;
}

pci::LoadJobRow
job(const std::uint64_t id, const QString &detail, const double completion)
{
    return {
        .key = {.kind = pci::LoadJobKind::Vector, .id = pci::LoadJobId{id}},
        .title = QStringLiteral("Vector import %1").arg(id),
        .detail = detail,
        .completion = completion,
        .capabilities = {.canCancel = true, .canPrioritize = true},
    };
}

} // namespace

TEST_CASE("scene layer model reconciles snapshots incrementally",
          "[ui][models][layers]")
{
    pci::SceneLayerListModel model;
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    const auto first = pointLayer(1, "first.las", 1'200);
    const auto second = pointLayer(2, "second.las", 2'000'000);
    model.setSnapshot(snapshot({first, second}));
    REQUIRE(model.rowCount() == 2);
    CHECK(model.index(0).data().toString() == QStringLiteral("first.las"));
    CHECK(
        model.index(0).data(pci::SceneLayerListModel::SummaryRole).toString() ==
        QStringLiteral("1.2K"));
    CHECK(model.index(1)
              .data(pci::SceneLayerListModel::LayerIdRole)
              .value<pci::SceneLayerId>() == second.id);

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.setSnapshot(snapshot({first, second}));
    CHECK(inserted.isEmpty());
    CHECK(removed.isEmpty());
    CHECK(moved.isEmpty());
    CHECK(changed.isEmpty());
    CHECK(reset.isEmpty());

    auto hidden = first;
    hidden.visible = false;
    model.setSnapshot(snapshot({second, hidden}));
    CHECK(moved.count() == 1);
    CHECK(changed.count() == 1);
    CHECK(model.index(1).data(Qt::CheckStateRole).toInt() == Qt::Unchecked);
    CHECK(model.rowForId(first.id) == 1);
}

TEST_CASE("scene layer model exposes baked raster color provenance",
          "[ui][models][layers][raster][colorize]")
{
    pci::SceneLayerListModel model;
    auto point = pointLayer(4, "/data/cloud.laz", 25);
    point.rasterColors = pci::RasterPointColorBinding{
        .rasterLayerId = pci::SceneLayerId{9},
        .rasterSourcePath = "/data/ortho.tif",
        .coloredPoints = 20,
        .uncoloredPoints = 5,
    };

    model.setSnapshot(snapshot({point}));
    const QModelIndex index = model.index(0, 0);
    CHECK(index.data(pci::SceneLayerListModel::RasterColorsRole).toBool());
    CHECK(index.data(pci::SceneLayerListModel::RasterColorSourceRole)
              .toString() == QStringLiteral("ortho.tif"));
    CHECK(index.data(Qt::ToolTipRole)
              .toString()
              .contains(QStringLiteral("Raster colors: ortho.tif · linked")));
    CHECK(model.roleNames().value(pci::SceneLayerListModel::RasterColorsRole) ==
          "rasterColors");
}

TEST_CASE("task model reconciles stable job keys without resets",
          "[ui][models][tasks]")
{
    pci::TaskListModel model;
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    const auto first = job(1, QStringLiteral("Queued"), 0.0);
    const auto second = job(2, QStringLiteral("Reading"), 0.25);
    model.setRows({first, second});

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.setRows({first, second});
    CHECK(inserted.isEmpty());
    CHECK(removed.isEmpty());
    CHECK(moved.isEmpty());
    CHECK(changed.isEmpty());
    CHECK(reset.isEmpty());

    auto progressed = first;
    progressed.detail = QStringLiteral("Indexing");
    progressed.completion = 0.5;
    model.setRows({second, progressed});
    CHECK(moved.count() == 1);
    CHECK(changed.count() == 1);
    CHECK(model.rowForKey(first.key) == 1);
    CHECK(model.index(1).data(pci::TaskListModel::DetailRole).toString() ==
          QStringLiteral("Indexing"));
}

TEST_CASE("dock selection and task widgets survive unrelated updates",
          "[ui][models][docks]")
{
    const auto first = pointLayer(1, "first.las");
    const auto second = pointLayer(2, "second.las");
    pci::SceneLayersDock layers;
    layers.setDocumentSnapshot(snapshot({first, second}));
    auto *layerView =
        layers.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    REQUIRE(layerView != nullptr);
    layerView->setCurrentIndex(layerView->model()->index(1, 0));
    layers.setDocumentSnapshot(snapshot({second, first}));
    CHECK(layers.currentLayerId() == second.id);
    CHECK(layerView->currentIndex().row() == 0);

    pci::TaskDock tasks;
    const auto task = job(7, QStringLiteral("Queued"), 0.0);
    tasks.setRows({task});
    QWidget *rowWidget =
        tasks.findChild<QWidget *>(QStringLiteral("loadTaskRow"));
    REQUIRE(rowWidget != nullptr);
    tasks.setRows({task});
    CHECK(tasks.findChild<QWidget *>(QStringLiteral("loadTaskRow")) ==
          rowWidget);
}

TEST_CASE("scene layer model projects raster rows with dimensions",
          "[ui][models][layers][raster]")
{
    pci::SceneLayerListModel model;
    const QAbstractItemModelTester tester(&model);

    model.setSnapshot(rasterSnapshot({
        pci::RasterLayer{.id = pci::SceneLayerId{7},
                         .data = rasterData("/data/ortho.tif", 2048, 1536)},
    }));
    REQUIRE(model.rowCount() == 1);

    const QModelIndex index = model.index(0, 0);
    CHECK(index.data(pci::SceneLayerListModel::LayerKindRole).toInt() ==
          static_cast<int>(pci::SceneLayerKind::Raster));
    CHECK(index.data(pci::SceneLayerListModel::NameRole).toString() ==
          QStringLiteral("ortho.tif"));
    // Dimensions are the summary a raster user recognizes.
    CHECK(index.data(pci::SceneLayerListModel::SummaryRole).toString() ==
          QStringLiteral("2048 x 1536"));
    CHECK_FALSE(index.data(pci::SceneLayerListModel::WarningRole).toBool());
    CHECK_FALSE(index.data(pci::SceneLayerListModel::ShowAnywayRole).toBool());
}

TEST_CASE("scene layer model warns about unusable raster placement",
          "[ui][models][layers][raster]")
{
    pci::SceneLayerListModel model;

    SECTION("a disjoint extent offers Show anyway once hidden")
    {
        pci::RasterLayerDataPtr data = rasterData("/data/elsewhere.tif");
        const_cast<pci::RasterLayerMetadata &>(data->metadata())
            .extentDisjointXY = true;
        // The controller hides a disjoint raster on arrival, which is the
        // state in which the affordance is offered.
        model.setSnapshot(rasterSnapshot({
            pci::RasterLayer{
                .id = pci::SceneLayerId{1}, .data = data, .visible = false},
        }));
        const QModelIndex index = model.index(0, 0);
        CHECK(index.data(pci::SceneLayerListModel::WarningRole).toBool());
        CHECK(index.data(pci::SceneLayerListModel::ShowAnywayRole).toBool());

        // Once shown, the affordance disappears but the warning remains.
        model.setSnapshot(rasterSnapshot({
            pci::RasterLayer{
                .id = pci::SceneLayerId{1}, .data = data, .visible = true},
        }));
        CHECK(model.index(0, 0)
                  .data(pci::SceneLayerListModel::WarningRole)
                  .toBool());
        CHECK_FALSE(model.index(0, 0)
                        .data(pci::SceneLayerListModel::ShowAnywayRole)
                        .toBool());
    }

    SECTION("insufficient overviews warn without offering Show anyway")
    {
        pci::RasterLayerDataPtr data = rasterData("/data/huge.tif");
        const_cast<pci::RasterLayerMetadata &>(data->metadata())
            .insufficientOverviews = true;
        model.setSnapshot(rasterSnapshot({
            pci::RasterLayer{.id = pci::SceneLayerId{2}, .data = data},
        }));
        const QModelIndex index = model.index(0, 0);
        // Display quality is bounded by what the dataset provides; the
        // application reports that rather than compensating for it.
        CHECK(index.data(pci::SceneLayerListModel::WarningRole).toBool());
        CHECK_FALSE(
            index.data(pci::SceneLayerListModel::ShowAnywayRole).toBool());
    }

    SECTION("a missing CRS is a warning, not a rejection")
    {
        pci::RasterLayerDataPtr data = rasterData("/data/no-crs.tif");
        const_cast<pci::RasterLayerMetadata &>(data->metadata()).crsMissing =
            true;
        model.setSnapshot(rasterSnapshot({
            pci::RasterLayer{.id = pci::SceneLayerId{3}, .data = data},
        }));
        CHECK(model.rowCount() == 1);
        CHECK(model.index(0, 0)
                  .data(pci::SceneLayerListModel::WarningRole)
                  .toBool());
    }

    SECTION("a positional RGB assignment is a warning")
    {
        pci::RasterLayerDataPtr data = rasterData("/data/unlabelled.tif");
        const_cast<pci::RasterLayerMetadata &>(data->metadata())
            .positionalBandFallback = true;
        model.setSnapshot(rasterSnapshot({
            pci::RasterLayer{.id = pci::SceneLayerId{4}, .data = data},
        }));
        CHECK(model.index(0, 0)
                  .data(pci::SceneLayerListModel::WarningRole)
                  .toBool());
    }
}

TEST_CASE("scene layer model mixes point, vector, and raster rows in order",
          "[ui][models][layers][raster]")
{
    pci::SceneLayerListModel model;
    auto combined = std::make_shared<pci::SceneDocumentSnapshot>();

    pci::PointCloudMetadata pointMetadata;
    pointMetadata.sourcePath = "/data/cloud.las";
    combined->layers.push_back(
        {.id = pci::SceneLayerId{1},
         .payload = pci::PointCloudLayerState{
             .scene = std::make_shared<pci::PointCloudScene>(pointMetadata)}});

    auto vector = std::make_shared<pci::VectorLayerData>();
    vector->sourcePath = "/data/roads.gpkg";
    vector->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 0.0}};
    combined->layers.push_back(
        {.id = pci::SceneLayerId{2},
         .payload = pci::VectorLayerState{.data = vector}});

    combined->layers.push_back({.id = pci::SceneLayerId{3},
                                .payload = pci::RasterLayerState{
                                    .data = rasterData("/data/ortho.tif")}});

    model.setSnapshot(combined);
    REQUIRE(model.rowCount() == 3);
    CHECK(model.index(0, 0)
              .data(pci::SceneLayerListModel::LayerKindRole)
              .toInt() == static_cast<int>(pci::SceneLayerKind::PointCloud));
    CHECK(model.index(1, 0)
              .data(pci::SceneLayerListModel::LayerKindRole)
              .toInt() == static_cast<int>(pci::SceneLayerKind::Vector));
    CHECK(model.index(2, 0)
              .data(pci::SceneLayerListModel::LayerKindRole)
              .toInt() == static_cast<int>(pci::SceneLayerKind::Raster));
}
