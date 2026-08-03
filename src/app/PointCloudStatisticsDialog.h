#pragma once

#include "import/PointCloudStatistics.h"
#include "pointcloud/PointCloudMetadata.h"

#include <QDialog>

#include <atomic>
#include <memory>
#include <stop_token>

class QLabel;
class QProgressBar;
class QTableWidget;
class QTimer;

template <typename T> class QFutureWatcher;

namespace pci {

class PointCloudStatisticsDialog final : public QDialog {
public:
    explicit PointCloudStatisticsDialog(
        PointCloudMetadata metadata,
        std::shared_ptr<const PointCloudStatisticsProvider> statisticsProvider,
        QWidget *parent = nullptr);
    ~PointCloudStatisticsDialog() override;

    void startAnalysis();

private:
    void showMetadata();
    void showStatistics(const PointCloudStatistics &statistics);
    void showFailure(const QString &message);
    void updateProgress();

    PointCloudMetadata metadata_;
    std::shared_ptr<const PointCloudStatisticsProvider> statisticsProvider_;
    QTableWidget *coordinateTable_ = nullptr;
    QTableWidget *attributeTable_ = nullptr;
    QTableWidget *classificationTable_ = nullptr;
    QTableWidget *returnTable_ = nullptr;
    QTableWidget *numberOfReturnsTable_ = nullptr;
    QLabel *pointCountValue_ = nullptr;
    QLabel *scannedPointCountValue_ = nullptr;
    QLabel *extentValue_ = nullptr;
    QLabel *centreValue_ = nullptr;
    QLabel *areaValue_ = nullptr;
    QLabel *volumeValue_ = nullptr;
    QLabel *horizontalDensityValue_ = nullptr;
    QLabel *volumetricDensityValue_ = nullptr;
    QLabel *spacingValue_ = nullptr;
    QLabel *outlierMethodValue_ = nullptr;
    QLabel *outlierSampleValue_ = nullptr;
    QLabel *outlierMeanValue_ = nullptr;
    QLabel *outlierDeviationValue_ = nullptr;
    QLabel *outlierThresholdValue_ = nullptr;
    QLabel *outlierSampleCountValue_ = nullptr;
    QLabel *outlierEstimateValue_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QTimer *progressTimer_ = nullptr;
    QFutureWatcher<PointCloudStatistics> *watcher_ = nullptr;
    std::stop_source stopSource_;
    std::shared_ptr<std::atomic_uint64_t> processed_;
    bool analysisStarted_ = false;
};

} // namespace pci
