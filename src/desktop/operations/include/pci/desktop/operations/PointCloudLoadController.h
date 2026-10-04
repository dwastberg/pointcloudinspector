#pragma once
#include <QObject>
#include <QTimer>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/PointImportOperation.h>
namespace pci {
using PointCloudLoadControllerMetrics = PointImportOperationMetrics;
class PointCloudLoadController final
    : public QObject
    , public PointImportOperation {
    Q_OBJECT
public:
    PointCloudLoadController(std::shared_ptr<const PointCloudLoader> loader,
                             TaskScheduler &scheduler,
                             QObject *parent = nullptr);
    ~PointCloudLoadController() override = default;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    void rejectInstallation(LoadJobId id, const QString &message)
    {
        PointImportOperation::rejectInstallation(id, message.toStdString());
    }
    PointCloudLoadControllerMetrics metrics() const;
signals:
    void progressChanged(pci::LoadJobId jobId,
                         pci::PointCloudImportStage stage,
                         quint64 processed,
                         quint64 total);
    // Installation consumers run synchronously on this controller's owner
    // thread. Retain only data admitted into their runtime, not queued events.
    void dataReady(pci::LoadJobId jobId, pci::PointDatasetEvent event);
    void prepared(pci::LoadJobId jobId, pci::PreparedPointDatasetPtr dataset);
    void loaded(pci::LoadJobId jobId, pci::PreparedPointDatasetPtr dataset);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void schedulingChanged();
    void jobStateChanged(pci::LoadJobId jobId);

private:
    QTimer recoveryTimer_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::PreparedPointDatasetPtr)
Q_DECLARE_METATYPE(pci::PointDatasetEvent)
Q_DECLARE_METATYPE(pci::PointCloudImportStage)
