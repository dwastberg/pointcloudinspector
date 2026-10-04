#pragma once
#include <QObject>
#include <QTimer>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/RasterColorizeOperation.h>
namespace pci {
using PointCloudColorizeControllerMetrics = RasterColorizeOperationMetrics;
class PointCloudColorizeController final
    : public QObject
    , public RasterColorizeOperation {
    Q_OBJECT
public:
    PointCloudColorizeController(TaskScheduler &scheduler,
                                 RasterColorizeRunStoreFactory runStoreFactory,
                                 QObject *parent = nullptr);
    ~PointCloudColorizeController() override = default;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    void finishCommit(LoadJobId id, bool applied, QString message = {})
    {
        RasterColorizeOperation::finishCommit(
            id, applied, message.toStdString());
    }
signals:
    void prepared(pci::LoadJobId jobId,
                  pci::RasterColorizeCommitToken token,
                  pci::PointColorInstallationPtr installation,
                  pci::RasterPointColorBinding binding);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    QTimer recoveryTimer_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::RasterColorizeCommitToken)
Q_DECLARE_METATYPE(pci::PointColorInstallationPtr)
Q_DECLARE_METATYPE(pci::RasterPointColorBinding)
