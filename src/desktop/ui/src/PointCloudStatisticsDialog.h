#pragma once

#include <pci/operations/StatisticsOperation.h>
#include <pci/pointcloud/PointCloudMetadata.h>

#include <QDialog>

#include <memory>

class QLabel;
class QProgressBar;
class QTableWidget;
class QTimer;

namespace pci {

class PointCloudStatisticsDialog final : public QDialog {
public:
    using StartAnalysis =
        std::function<StatisticsSubscription(StatisticsSubscription::Observer)>;
    explicit PointCloudStatisticsDialog(PointCloudMetadata metadata,
                                        StartAnalysis startAnalysis,
                                        QWidget *parent = nullptr);
    ~PointCloudStatisticsDialog() override;

    void startAnalysis();

private:
    void showMetadata();
    void showStatistics(const PointCloudStatistics &statistics);
    void showFailure(const QString &message);
    void updateProgress();

    PointCloudMetadata metadata_;
    StartAnalysis start_;
    StatisticsSubscription subscription_;
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
    std::uint64_t processed_ = 0;
    bool analysisStarted_ = false;
};

} // namespace pci
