#include "app/SceneLayerListModel.h"

#include "platform/QtPath.h"

#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

QString compactCount(const std::uint64_t count)
{
    if (count >= 1'000'000) {
        return QStringLiteral("%1M").arg(
            static_cast<double>(count) / 1'000'000.0, 0, 'f', 1);
    }
    if (count >= 1'000) {
        return QStringLiteral("%1K").arg(
            static_cast<double>(count) / 1'000.0, 0, 'f', 1);
    }
    return QString::number(count);
}

QString pointName(const PointCloudLayer &layer)
{
    const std::filesystem::path &path = layer.scene->metadata().sourcePath;
    return path.empty() ? QStringLiteral("Point cloud %1").arg(layer.id.value())
                        : displayPathName(path);
}

QString rasterName(const RasterLayer &layer)
{
    if (!layer.data) {
        return QStringLiteral("Raster layer %1").arg(layer.id.value());
    }
    const std::filesystem::path &path = layer.data->metadata().sourcePath;
    return path.empty()
               ? QStringLiteral("Raster layer %1").arg(layer.id.value())
               : displayPathName(path);
}

// Dimensions are the summary a raster user recognizes; a pixel count is not
// how imagery is described.
QString rasterSummary(const RasterLayer &layer)
{
    if (!layer.data) {
        return {};
    }
    const RasterLayerMetadata &metadata = layer.data->metadata();
    return QStringLiteral("%1 x %2").arg(metadata.width).arg(metadata.height);
}

QString vectorName(const VectorLayer &layer)
{
    if (!layer.data) {
        return QStringLiteral("Vector layer %1").arg(layer.id.value());
    }
    if (!layer.data->sublayerName.empty()) {
        return QString::fromStdString(layer.data->sublayerName);
    }
    return layer.data->sourcePath.filename().empty()
               ? QStringLiteral("Vector layer %1").arg(layer.id.value())
               : displayPathName(layer.data->sourcePath);
}

} // namespace

SceneLayerListModel::SceneLayerListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int SceneLayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SceneLayerListModel::data(const QModelIndex &index,
                                   const int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row &row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return row.name;
    case Qt::CheckStateRole:
        return row.visible ? Qt::Checked : Qt::Unchecked;
    case VisibilityRole:
        return row.visible;
    case Qt::ToolTipRole:
    case SourceToolTipRole:
        return row.toolTip;
    case LayerIdRole:
        return QVariant::fromValue(row.id);
    case SummaryRole:
        return row.summary;
    case LayerKindRole:
        return static_cast<int>(row.kind);
    case WarningRole:
        return row.warning;
    case ShowAnywayRole:
        return row.allowShowAnyway && !row.visible;
    case RasterColorsRole:
        return row.rasterColors;
    case RasterColorSourceRole:
        return row.rasterColorSource;
    default:
        return {};
    }
}

Qt::ItemFlags SceneLayerListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return QAbstractListModel::flags(index) | Qt::ItemIsUserCheckable;
}

bool SceneLayerListModel::setData(const QModelIndex &index,
                                  const QVariant &value,
                                  const int role)
{
    if (role != Qt::CheckStateRole || !index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size())) {
        return false;
    }
    Row &row = rows_[static_cast<std::size_t>(index.row())];
    const bool visible = value.toInt() == Qt::Checked;
    if (row.visible == visible) {
        return false;
    }
    row.visible = visible;
    emit dataChanged(
        index, index, {Qt::CheckStateRole, VisibilityRole, ShowAnywayRole});
    emit visibilityEditRequested(row.id, visible);
    return true;
}

QHash<int, QByteArray> SceneLayerListModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(LayerIdRole, "layerId");
    roles.insert(NameRole, "name");
    roles.insert(VisibilityRole, "visible");
    roles.insert(SummaryRole, "summary");
    roles.insert(LayerKindRole, "layerKind");
    roles.insert(WarningRole, "warning");
    roles.insert(ShowAnywayRole, "showAnyway");
    roles.insert(SourceToolTipRole, "sourceToolTip");
    roles.insert(RasterColorsRole, "rasterColors");
    roles.insert(RasterColorSourceRole, "rasterColorSource");
    return roles;
}

void SceneLayerListModel::setSnapshot(SceneDocumentSnapshotPtr snapshot)
{
    if (!snapshot) {
        throw std::invalid_argument("scene layer model requires a snapshot");
    }
    reconcile(project(*snapshot));
}

std::optional<int> SceneLayerListModel::rowForId(const SceneLayerId id) const
{
    const auto found = std::ranges::find(rows_, id, &Row::id);
    if (found == rows_.end()) {
        return std::nullopt;
    }
    return static_cast<int>(std::distance(rows_.begin(), found));
}

std::optional<SceneLayerId> SceneLayerListModel::idForRow(const int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return std::nullopt;
    }
    return rows_[static_cast<std::size_t>(row)].id;
}

std::vector<SceneLayerListModel::Row>
SceneLayerListModel::project(const SceneDocumentSnapshot &snapshot)
{
    std::vector<Row> result;
    result.reserve(snapshot.layers.size());
    for (const SceneLayer &sceneLayer : snapshot.layers) {
        if (const auto *point =
                std::get_if<PointCloudLayerState>(&sceneLayer.payload)) {
            const PointCloudLayer layer{
                .id = sceneLayer.id,
                .scene = point->scene,
                .visible = sceneLayer.visible,
                .colorMode = point->colorMode,
                .classificationFilter = point->classificationFilter,
                .rasterColors = point->rasterColors,
                .colorGeneration = point->colorGeneration};
            const std::uint64_t resident = layer.scene->totalPointCount();
            const QString pointSource =
                pathToQString(layer.scene->metadata().sourcePath);
            QString rasterColorSource;
            QString toolTip = pointSource;
            if (layer.rasterColors) {
                rasterColorSource =
                    layer.rasterColors->rasterSourcePath.empty()
                        ? QStringLiteral("Raster source")
                        : displayPathName(layer.rasterColors->rasterSourcePath);
                if (!toolTip.isEmpty()) {
                    toolTip += QChar::LineFeed;
                }
                toolTip +=
                    QStringLiteral("Raster colors: %1 · %2")
                        .arg(rasterColorSource)
                        .arg(layer.rasterColors->rasterLayerId
                                 ? QStringLiteral("linked")
                                 : QStringLiteral("source layer removed"));
            }
            result.push_back({
                .id = sceneLayer.id,
                .kind = SceneLayerKind::PointCloud,
                .name = pointName(layer),
                .summary = compactCount(
                    resident > 0 ? resident
                                 : layer.scene->metadata().sourcePointCount),
                .toolTip = std::move(toolTip),
                .rasterColorSource = std::move(rasterColorSource),
                .visible = sceneLayer.visible,
                .rasterColors = layer.rasterColors.has_value(),
            });
        } else if (const auto *vector =
                       std::get_if<VectorLayerState>(&sceneLayer.payload)) {
            const VectorLayer layer{.id = sceneLayer.id,
                                    .data = vector->data,
                                    .visible = sceneLayer.visible,
                                    .style = vector->style};
            result.push_back({
                .id = sceneLayer.id,
                .kind = SceneLayerKind::Vector,
                .name = vectorName(layer),
                .summary =
                    compactCount(layer.data ? layer.data->featureCount : 0),
                .toolTip = layer.data ? pathToQString(layer.data->sourcePath)
                                      : QString{},
                .rasterColorSource = {},
                .visible = sceneLayer.visible,
                .rasterColors = false,
                .warning = layer.data && (layer.data->extentDisjointXY ||
                                          layer.data->crsMismatch),
                .allowShowAnyway = layer.data && layer.data->extentDisjointXY,
            });
        } else if (const auto *raster =
                       std::get_if<RasterLayerState>(&sceneLayer.payload)) {
            const RasterLayer layer{.id = sceneLayer.id,
                                    .data = raster->data,
                                    .visible = sceneLayer.visible,
                                    .style = raster->style,
                                    .renderGeneration =
                                        raster->renderGeneration,
                                    .elevationStatus = raster->elevationStatus,
                                    .exactElevationRange =
                                        raster->exactElevationRange,
                                    .elevationFailure =
                                        raster->elevationFailure,
                                    .elevationGeneration =
                                        raster->elevationGeneration};
            const RasterLayerMetadata *metadata =
                layer.data ? &layer.data->metadata() : nullptr;
            result.push_back({
                .id = sceneLayer.id,
                .kind = SceneLayerKind::Raster,
                .name = rasterName(layer),
                .summary = rasterSummary(layer),
                .toolTip =
                    metadata ? pathToQString(metadata->sourcePath) : QString{},
                .rasterColorSource = {},
                .visible = sceneLayer.visible,
                .rasterColors = false,
                // Source overview quality still merits a warning even though
                // an in-memory coverage preview keeps the whole raster shown.
                .warning =
                    metadata != nullptr &&
                    (metadata->extentDisjointXY || metadata->crsMismatch ||
                     metadata->crsMissing || metadata->insufficientOverviews ||
                     metadata->positionalBandFallback),
                .allowShowAnyway =
                    metadata != nullptr && metadata->extentDisjointXY,
            });
        }
    }
    return result;
}

void SceneLayerListModel::reconcile(std::vector<Row> next)
{
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (std::ranges::none_of(next, [id = rows_[row].id](const Row &entry) {
                return entry.id == id;
            })) {
            beginRemoveRows({}, row, row);
            rows_.erase(rows_.begin() + row);
            endRemoveRows();
        }
    }
    for (int target = 0; target < static_cast<int>(next.size()); ++target) {
        const Row &desired = next[static_cast<std::size_t>(target)];
        if (target >= static_cast<int>(rows_.size())) {
            beginInsertRows({}, target, target);
            rows_.insert(rows_.begin() + target, desired);
            endInsertRows();
        } else if (rows_[static_cast<std::size_t>(target)].id != desired.id) {
            const auto found = std::ranges::find(
                rows_.begin() + target + 1, rows_.end(), desired.id, &Row::id);
            if (found == rows_.end()) {
                beginInsertRows({}, target, target);
                rows_.insert(rows_.begin() + target, desired);
                endInsertRows();
            } else {
                const int source =
                    static_cast<int>(std::distance(rows_.begin(), found));
                beginMoveRows({}, source, source, {}, target);
                Row moved = std::move(rows_[static_cast<std::size_t>(source)]);
                rows_.erase(rows_.begin() + source);
                rows_.insert(rows_.begin() + target, std::move(moved));
                endMoveRows();
            }
        }
        Row &current = rows_[static_cast<std::size_t>(target)];
        if (current != desired) {
            current = desired;
            const QModelIndex changed = index(target);
            emit dataChanged(changed, changed);
        }
    }
}

} // namespace pci
