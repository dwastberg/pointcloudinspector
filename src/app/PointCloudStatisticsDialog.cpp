#include "app/PointCloudStatisticsDialog.h"

#include "foundation/CheckedArithmetic.h"
#include "platform/QtPath.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace pci {
namespace {

QString number(const double value)
{
    if (!std::isfinite(value)) {
        return QStringLiteral("—");
    }
    const double magnitude = std::abs(value);
    const char format =
        (magnitude != 0.0 && (magnitude >= 1.0e8 || magnitude < 1.0e-4)) ? 'g'
                                                                         : 'f';
    return QString::number(value, format, format == 'g' ? 8 : 4);
}

QString count(const std::uint64_t value)
{
    return QStringLiteral("%L1").arg(static_cast<qulonglong>(value));
}

QString optionalNumber(const std::optional<double> value,
                       const QString &suffix = {})
{
    return value ? number(*value) + suffix
                 : QStringLiteral("Not defined for degenerate bounds");
}

QLabel *makeValueLabel(QWidget *parent, const QString &objectName)
{
    auto *label = new QLabel(QStringLiteral("—"), parent);
    label->setObjectName(objectName);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

void prepareStatisticsTable(QTableWidget *table, const QStringList &headers)
{
    table->setColumnCount(static_cast<int>(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setAlternatingRowColors(true);
}

void setCell(QTableWidget *table,
             const int row,
             const int column,
             const QString &value)
{
    auto *item = new QTableWidgetItem(value);
    if (column > 0) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    table->setItem(row, column, item);
}

void setNumericRow(QTableWidget *table,
                   const int row,
                   const QString &name,
                   const NumericPointStatistics &statistics)
{
    setCell(table, row, 0, name);
    setCell(table, row, 1, number(statistics.minimum));
    setCell(table, row, 2, number(statistics.maximum));
    setCell(table, row, 3, number(statistics.mean));
    setCell(table, row, 4, number(statistics.standardDeviation));
    setCell(table, row, 5, number(statistics.maximum - statistics.minimum));
}

template <std::size_t Size>
void populateCounts(QTableWidget *table,
                    const std::array<std::uint64_t, Size> &counts,
                    const std::uint64_t total)
{
    table->setRowCount(0);
    for (std::size_t value = 0; value < counts.size(); ++value) {
        if (counts[value] == 0) {
            continue;
        }
        const int row = table->rowCount();
        table->insertRow(row);
        setCell(table, row, 0, QString::number(value));
        setCell(table, row, 1, count(counts[value]));
        const double percentage = total > 0
                                      ? static_cast<double>(counts[value]) *
                                            100.0 / static_cast<double>(total)
                                      : 0.0;
        setCell(table, row, 2, QStringLiteral("%1%").arg(number(percentage)));
    }
}

} // namespace

PointCloudStatisticsDialog::PointCloudStatisticsDialog(
    PointCloudMetadata metadata,
    std::shared_ptr<const PointCloudStatisticsProvider> statisticsProvider,
    QWidget *parent)
    : QDialog(parent)
    , metadata_(std::move(metadata))
    , statisticsProvider_(std::move(statisticsProvider))
    , processed_(std::make_shared<std::atomic_uint64_t>(0))
{
    if (!statisticsProvider_) {
        throw std::invalid_argument(
            "point-cloud statistics dialog requires a provider");
    }
    setObjectName(QStringLiteral("pointCloudStatisticsDialog"));
    setWindowTitle(
        QStringLiteral("Point Cloud Statistics — %1")
            .arg(QFileInfo(pathToQString(metadata_.sourcePath)).fileName()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 600);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("pointCloudStatisticsTabs"));

    auto *summary = new QWidget(tabs);
    auto *summaryLayout = new QFormLayout(summary);
    summaryLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    pointCountValue_ =
        makeValueLabel(summary, QStringLiteral("statisticsPointCountValue"));
    scannedPointCountValue_ = makeValueLabel(
        summary, QStringLiteral("statisticsScannedPointCountValue"));
    extentValue_ =
        makeValueLabel(summary, QStringLiteral("statisticsExtentValue"));
    centreValue_ =
        makeValueLabel(summary, QStringLiteral("statisticsCentreValue"));
    areaValue_ = makeValueLabel(summary, QStringLiteral("statisticsAreaValue"));
    volumeValue_ =
        makeValueLabel(summary, QStringLiteral("statisticsVolumeValue"));
    horizontalDensityValue_ = makeValueLabel(
        summary, QStringLiteral("statisticsHorizontalDensityValue"));
    volumetricDensityValue_ = makeValueLabel(
        summary, QStringLiteral("statisticsVolumetricDensityValue"));
    spacingValue_ =
        makeValueLabel(summary, QStringLiteral("statisticsSpacingValue"));
    summaryLayout->addRow(QStringLiteral("Source points"), pointCountValue_);
    summaryLayout->addRow(QStringLiteral("Points scanned"),
                          scannedPointCountValue_);
    summaryLayout->addRow(QStringLiteral("Extent (X × Y × Z)"), extentValue_);
    summaryLayout->addRow(QStringLiteral("Centre (X, Y, Z)"), centreValue_);
    summaryLayout->addRow(QStringLiteral("XY bounding area"), areaValue_);
    summaryLayout->addRow(QStringLiteral("3D bounding volume"), volumeValue_);
    summaryLayout->addRow(QStringLiteral("XY bounding-box density"),
                          horizontalDensityValue_);
    summaryLayout->addRow(QStringLiteral("3D bounding-box density"),
                          volumetricDensityValue_);
    summaryLayout->addRow(QStringLiteral("Nominal horizontal spacing"),
                          spacingValue_);
    tabs->addTab(summary, QStringLiteral("Summary"));

    coordinateTable_ = new QTableWidget(tabs);
    coordinateTable_->setObjectName(
        QStringLiteral("statisticsCoordinateTable"));
    prepareStatisticsTable(coordinateTable_,
                           {QStringLiteral("Axis"),
                            QStringLiteral("Minimum"),
                            QStringLiteral("Maximum"),
                            QStringLiteral("Mean"),
                            QStringLiteral("Std. deviation"),
                            QStringLiteral("Span")});
    coordinateTable_->setRowCount(3);
    tabs->addTab(coordinateTable_, QStringLiteral("Coordinates"));

    auto *attributes = new QWidget(tabs);
    auto *attributesLayout = new QVBoxLayout(attributes);
    attributeTable_ = new QTableWidget(attributes);
    attributeTable_->setObjectName(QStringLiteral("statisticsAttributeTable"));
    prepareStatisticsTable(attributeTable_,
                           {QStringLiteral("Attribute"),
                            QStringLiteral("Minimum"),
                            QStringLiteral("Maximum"),
                            QStringLiteral("Mean"),
                            QStringLiteral("Std. deviation"),
                            QStringLiteral("Span")});
    attributeTable_->setMaximumHeight(130);
    attributesLayout->addWidget(
        new QLabel(QStringLiteral("Numeric attributes"), attributes));
    attributesLayout->addWidget(attributeTable_);
    classificationTable_ = new QTableWidget(attributes);
    classificationTable_->setObjectName(
        QStringLiteral("statisticsClassificationTable"));
    prepareStatisticsTable(classificationTable_,
                           {QStringLiteral("Classification"),
                            QStringLiteral("Points"),
                            QStringLiteral("Share")});
    attributesLayout->addWidget(
        new QLabel(QStringLiteral("Classification distribution"), attributes));
    attributesLayout->addWidget(classificationTable_);
    returnTable_ = new QTableWidget(attributes);
    returnTable_->setObjectName(QStringLiteral("statisticsReturnTable"));
    prepareStatisticsTable(returnTable_,
                           {QStringLiteral("Return number"),
                            QStringLiteral("Points"),
                            QStringLiteral("Share")});
    attributesLayout->addWidget(
        new QLabel(QStringLiteral("Return-number distribution"), attributes));
    attributesLayout->addWidget(returnTable_);
    numberOfReturnsTable_ = new QTableWidget(attributes);
    numberOfReturnsTable_->setObjectName(
        QStringLiteral("statisticsNumberOfReturnsTable"));
    prepareStatisticsTable(numberOfReturnsTable_,
                           {QStringLiteral("Returns per pulse"),
                            QStringLiteral("Points"),
                            QStringLiteral("Share")});
    attributesLayout->addWidget(new QLabel(
        QStringLiteral("Number-of-returns distribution"), attributes));
    attributesLayout->addWidget(numberOfReturnsTable_);
    tabs->addTab(attributes, QStringLiteral("Attributes"));

    auto *outliers = new QWidget(tabs);
    auto *outlierLayout = new QFormLayout(outliers);
    outlierLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    outlierMethodValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierMethodValue"));
    outlierSampleValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierSampleValue"));
    outlierMeanValue_ =
        makeValueLabel(outliers, QStringLiteral("statisticsOutlierMeanValue"));
    outlierDeviationValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierDeviationValue"));
    outlierThresholdValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierThresholdValue"));
    outlierSampleCountValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierSampleCountValue"));
    outlierEstimateValue_ = makeValueLabel(
        outliers, QStringLiteral("statisticsOutlierEstimateValue"));
    outlierLayout->addRow(QStringLiteral("Method"), outlierMethodValue_);
    outlierLayout->addRow(QStringLiteral("Representative sample"),
                          outlierSampleValue_);
    outlierLayout->addRow(QStringLiteral("Mean neighbour distance"),
                          outlierMeanValue_);
    outlierLayout->addRow(QStringLiteral("Distance std. deviation"),
                          outlierDeviationValue_);
    outlierLayout->addRow(QStringLiteral("Outlier threshold"),
                          outlierThresholdValue_);
    outlierLayout->addRow(QStringLiteral("Outliers in sample"),
                          outlierSampleCountValue_);
    outlierLayout->addRow(QStringLiteral("Estimated source outliers"),
                          outlierEstimateValue_);
    tabs->addTab(outliers, QStringLiteral("Outliers"));

    layout->addWidget(tabs, 1);
    statusLabel_ =
        new QLabel(QStringLiteral("Preparing source analysis…"), this);
    statusLabel_->setObjectName(QStringLiteral("statisticsStatusLabel"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("statisticsProgressBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    layout->addWidget(progressBar_);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(100);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        updateProgress();
    });
    showMetadata();
}

PointCloudStatisticsDialog::~PointCloudStatisticsDialog()
{
    stopSource_.request_stop();
}

void PointCloudStatisticsDialog::startAnalysis()
{
    if (analysisStarted_) {
        return;
    }
    analysisStarted_ = true;
    if (metadata_.sourcePath.empty() || metadata_.sourceDriver.empty()) {
        showFailure(QStringLiteral(
            "Full source statistics are unavailable for this in-memory "
            "point cloud."));
        return;
    }

    watcher_ = new QFutureWatcher<PointCloudStatistics>(this);
    connect(watcher_,
            &QFutureWatcher<PointCloudStatistics>::finished,
            this,
            [this] {
                progressTimer_->stop();
                try {
                    showStatistics(watcher_->result());
                } catch (const PointCloudStatisticsCancelled &) {
                    showFailure(
                        QStringLiteral("Statistics analysis cancelled."));
                } catch (const std::exception &error) {
                    showFailure(QString::fromUtf8(error.what()));
                }
            });
    const PointCloudMetadata metadata = metadata_;
    const auto statisticsProvider = statisticsProvider_;
    const std::stop_token stopToken = stopSource_.get_token();
    const auto processed = processed_;
    watcher_->setFuture(
        QtConcurrent::run([metadata, statisticsProvider, stopToken, processed] {
            return statisticsProvider->calculate(
                metadata,
                stopToken,
                [processed](const std::uint64_t value, const std::uint64_t) {
                    processed->store(value, std::memory_order_relaxed);
                });
        }));
    progressTimer_->start();
}

void PointCloudStatisticsDialog::showMetadata()
{
    pointCountValue_->setText(count(metadata_.sourcePointCount));
    scannedPointCountValue_->setText(QStringLiteral("Analysis pending"));
    const Bounds3d &bounds = metadata_.sourceBounds;
    const std::array<double, 3> extents{
        bounds.maximum[0] - bounds.minimum[0],
        bounds.maximum[1] - bounds.minimum[1],
        bounds.maximum[2] - bounds.minimum[2],
    };
    extentValue_->setText(
        QStringLiteral("%1 × %2 × %3")
            .arg(number(extents[0]), number(extents[1]), number(extents[2])));
    centreValue_->setText(
        QStringLiteral("%1, %2, %3")
            .arg(number((bounds.minimum[0] + bounds.maximum[0]) * 0.5),
                 number((bounds.minimum[1] + bounds.maximum[1]) * 0.5),
                 number((bounds.minimum[2] + bounds.maximum[2]) * 0.5)));
    const std::array<QString, 3> axes{
        QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
    for (int row = 0; row < 3; ++row) {
        const std::size_t axis = static_cast<std::size_t>(row);
        setCell(coordinateTable_, row, 0, axes[axis]);
        setCell(coordinateTable_, row, 1, number(bounds.minimum[axis]));
        setCell(coordinateTable_, row, 2, number(bounds.maximum[axis]));
        setCell(coordinateTable_, row, 3, QStringLiteral("Analysis pending"));
        setCell(coordinateTable_, row, 4, QStringLiteral("Analysis pending"));
        setCell(coordinateTable_, row, 5, number(extents[axis]));
    }
    const int numericAttributeCount =
        (metadata_.hasIntensity ? 1 : 0) + (metadata_.hasColor ? 3 : 0);
    attributeTable_->setRowCount(numericAttributeCount);
    int attributeRow = 0;
    if (metadata_.hasIntensity) {
        setCell(attributeTable_, attributeRow, 0, QStringLiteral("Intensity"));
        for (int column = 1; column < attributeTable_->columnCount();
             ++column) {
            setCell(attributeTable_,
                    attributeRow,
                    column,
                    QStringLiteral("Analysis pending"));
        }
        ++attributeRow;
    }
    for (const QString &name : {QStringLiteral("Red"),
                                QStringLiteral("Green"),
                                QStringLiteral("Blue")}) {
        if (!metadata_.hasColor) {
            break;
        }
        setCell(attributeTable_, attributeRow, 0, name);
        for (int column = 1; column < attributeTable_->columnCount();
             ++column) {
            setCell(attributeTable_,
                    attributeRow,
                    column,
                    QStringLiteral("Analysis pending"));
        }
        ++attributeRow;
    }
    classificationTable_->setRowCount(1);
    setCell(classificationTable_,
            0,
            0,
            metadata_.hasClassification ? QStringLiteral("Analysis pending")
                                        : QStringLiteral("Not present"));
    returnTable_->setRowCount(1);
    setCell(returnTable_,
            0,
            0,
            metadata_.hasReturnNumber ? QStringLiteral("Analysis pending")
                                      : QStringLiteral("Not present"));
    numberOfReturnsTable_->setRowCount(1);
    setCell(numberOfReturnsTable_,
            0,
            0,
            metadata_.hasNumberOfReturns ? QStringLiteral("Analysis pending")
                                         : QStringLiteral("Not present"));
    outlierMethodValue_->setText(QStringLiteral(
        "Mean distance to nearest neighbours; outliers exceed the sample "
        "mean by 3 standard deviations."));
    outlierSampleValue_->setText(QStringLiteral("Analysis pending"));
}

void PointCloudStatisticsDialog::showStatistics(
    const PointCloudStatistics &statistics)
{
    progressBar_->setValue(100);
    progressBar_->hide();
    statusLabel_->setText(QStringLiteral(
        "Complete. Density uses the axis-aligned source bounding box; "
        "outlier counts are sample-based estimates and their 95% interval "
        "reflects sampling uncertainty only."));
    scannedPointCountValue_->setText(count(statistics.scannedPointCount));
    setNumericRow(coordinateTable_, 0, QStringLiteral("X"), statistics.x);
    setNumericRow(coordinateTable_, 1, QStringLiteral("Y"), statistics.y);
    setNumericRow(coordinateTable_, 2, QStringLiteral("Z"), statistics.z);

    const double xExtent = statistics.x.maximum - statistics.x.minimum;
    const double yExtent = statistics.y.maximum - statistics.y.minimum;
    const double zExtent = statistics.z.maximum - statistics.z.minimum;
    extentValue_->setText(
        QStringLiteral("%1 × %2 × %3")
            .arg(number(xExtent), number(yExtent), number(zExtent)));
    centreValue_->setText(
        QStringLiteral("%1, %2, %3")
            .arg(number((statistics.x.minimum + statistics.x.maximum) * 0.5),
                 number((statistics.y.minimum + statistics.y.maximum) * 0.5),
                 number((statistics.z.minimum + statistics.z.maximum) * 0.5)));
    areaValue_->setText(optionalNumber(statistics.horizontalBoundingArea));
    volumeValue_->setText(optionalNumber(statistics.boundingVolume));
    horizontalDensityValue_->setText(optionalNumber(
        statistics.horizontalDensity, QStringLiteral(" points/unit²")));
    volumetricDensityValue_->setText(optionalNumber(
        statistics.volumetricDensity, QStringLiteral(" points/unit³")));
    spacingValue_->setText(optionalNumber(statistics.nominalHorizontalSpacing,
                                          QStringLiteral(" units")));

    const int numericAttributeCount =
        (statistics.intensity ? 1 : 0) + (statistics.red ? 3 : 0);
    attributeTable_->setRowCount(numericAttributeCount);
    int attributeRow = 0;
    if (statistics.intensity) {
        setNumericRow(attributeTable_,
                      attributeRow,
                      QStringLiteral("Intensity"),
                      *statistics.intensity);
        ++attributeRow;
    }
    const std::array<std::pair<QString, const NumericPointStatistics *>, 3>
        colors{{
            {QStringLiteral("Red"),
             statistics.red ? &*statistics.red : nullptr},
            {QStringLiteral("Green"),
             statistics.green ? &*statistics.green : nullptr},
            {QStringLiteral("Blue"),
             statistics.blue ? &*statistics.blue : nullptr},
        }};
    for (const auto &[name, color] : colors) {
        if (!color) {
            continue;
        }
        setNumericRow(attributeTable_, attributeRow, name, *color);
        ++attributeRow;
    }
    if (statistics.hasClassification) {
        populateCounts(classificationTable_,
                       statistics.classificationCounts,
                       statistics.scannedPointCount);
    } else {
        classificationTable_->setRowCount(1);
        setCell(classificationTable_, 0, 0, QStringLiteral("Not present"));
    }
    if (statistics.hasReturnNumber) {
        populateCounts(returnTable_,
                       statistics.returnNumberCounts,
                       statistics.scannedPointCount);
    } else {
        returnTable_->setRowCount(1);
        setCell(returnTable_, 0, 0, QStringLiteral("Not present"));
    }
    if (statistics.hasNumberOfReturns) {
        populateCounts(numberOfReturnsTable_,
                       statistics.numberOfReturnsCounts,
                       statistics.scannedPointCount);
    } else {
        numberOfReturnsTable_->setRowCount(1);
        setCell(numberOfReturnsTable_, 0, 0, QStringLiteral("Not present"));
    }

    if (!statistics.spatialOutliers) {
        outlierSampleValue_->setText(QStringLiteral("Not enough points"));
        outlierMeanValue_->setText(QStringLiteral("—"));
        outlierDeviationValue_->setText(QStringLiteral("—"));
        outlierThresholdValue_->setText(QStringLiteral("—"));
        outlierSampleCountValue_->setText(QStringLiteral("—"));
        outlierEstimateValue_->setText(QStringLiteral("—"));
        return;
    }
    const SpatialOutlierStatistics &outliers = *statistics.spatialOutliers;
    outlierMethodValue_->setText(
        QStringLiteral(
            "Mean distance to %1 nearest sampled neighbours; flagged above "
            "mean + 3 standard deviations.")
            .arg(outliers.neighbourCount));
    outlierSampleValue_->setText(QStringLiteral("%1 of %2 points")
                                     .arg(count(outliers.samplePointCount),
                                          count(statistics.scannedPointCount)));
    outlierMeanValue_->setText(number(outliers.meanNeighbourDistance));
    outlierDeviationValue_->setText(
        number(outliers.neighbourDistanceStandardDeviation));
    outlierThresholdValue_->setText(number(outliers.distanceThreshold));
    outlierSampleCountValue_->setText(
        QStringLiteral("%1 (%2%)")
            .arg(count(outliers.sampleOutlierCount),
                 number(outliers.estimatedPercentage)));
    outlierEstimateValue_->setText(
        QStringLiteral("%1 (%2%; 95% interval %3–%4)")
            .arg(count(outliers.estimatedSourceOutlierCount),
                 number(outliers.estimatedPercentage),
                 count(outliers.confidenceLowerCount),
                 count(outliers.confidenceUpperCount)));
}

void PointCloudStatisticsDialog::showFailure(const QString &message)
{
    progressTimer_->stop();
    progressBar_->hide();
    statusLabel_->setText(message);
}

void PointCloudStatisticsDialog::updateProgress()
{
    const std::uint64_t processed = processed_->load(std::memory_order_relaxed);
    const std::uint64_t total = metadata_.sourcePointCount;
    if (total == 0) {
        progressBar_->setRange(0, 0);
        return;
    }
    if (processed >= total) {
        progressBar_->setRange(0, 0);
        statusLabel_->setText(
            QStringLiteral("Computing spatial outlier statistics…"));
        return;
    }
    progressBar_->setRange(0, 100);
    const auto scaled = checkedMultiply(processed, std::uint64_t{100});
    const std::uint64_t percentage =
        scaled ? std::min<std::uint64_t>(99, *scaled / total) : 99;
    progressBar_->setValue(static_cast<int>(percentage));
    statusLabel_->setText(QStringLiteral("Scanning source… %1 of %2 points")
                              .arg(count(processed), count(total)));
}

} // namespace pci
