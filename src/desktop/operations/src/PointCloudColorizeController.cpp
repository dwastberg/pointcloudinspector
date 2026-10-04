#include "OperationRows.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/operations/PointCloudColorizeController.h>
namespace pci {
PointCloudColorizeController::PointCloudColorizeController(
    TaskScheduler &scheduler,
    RasterColorizeRunStoreFactory runStoreFactory,
    QObject *parent)
    : QObject(parent)
    , RasterColorizeOperation(
          scheduler, runStoreFactory, makeQtCompletionExecutor(this))
{
    qRegisterMetaType<LoadJobId>();
    qRegisterMetaType<RasterColorizeCommitToken>();
    qRegisterMetaType<PointColorInstallationPtr>();
    qRegisterMetaType<RasterPointColorBinding>();
    setEvents({
        .prepared =
            [this](pci::LoadJobId jobId,
                   pci::RasterColorizeCommitToken token,
                   pci::PointColorInstallationPtr installation,
                   pci::RasterPointColorBinding binding) {
                emit prepared(jobId, token, installation, binding);
            },
        .failed =
            [this](pci::LoadJobId jobId, std::string message) {
                emit failed(jobId, QString::fromStdString(message));
            },
        .cancelled =
            [this](pci::LoadJobId jobId) {
                emit cancelled(jobId);
            },
        .jobStateChanged =
            [this](pci::LoadJobId jobId) {
                emit jobStateChanged(jobId);
            },
    });
    recoveryTimer_.setInterval(20);
    connect(&recoveryTimer_, &QTimer::timeout, this, [this] {
        recoverDelivery();
    });
    recoveryTimer_.start();
}
std::vector<LoadJobRow> PointCloudColorizeController::jobRows() const
{
    return desktopOperationRows(RasterColorizeOperation::jobRows());
}
} // namespace pci
