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
            .payload = pci::PointCloudLayerState{.scene = layer.scene},
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
