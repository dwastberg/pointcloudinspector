#include "OperationRows.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/operations/RasterElevationController.h>
namespace pci {
RasterElevationController::RasterElevationController(TaskScheduler &scheduler,
                                                     QObject *parent)
    : QObject(parent)
    , RasterElevationOperation(scheduler, makeQtCompletionExecutor(this))
{
    qRegisterMetaType<RasterElevationRange>();
    qRegisterMetaType<RasterElevationBindingToken>();
    setEvents({
        .completed =
            [this](pci::LoadJobId jobId,
                   pci::RasterElevationBindingToken token,
                   pci::RasterElevationRange range) {
                emit completed(jobId, token, range);
            },
        .failed =
            [this](pci::LoadJobId jobId,
                   pci::RasterElevationBindingToken token,
                   std::string message) {
                emit failed(jobId, token, QString::fromStdString(message));
            },
        .cancelled =
            [this](pci::LoadJobId jobId,
                   pci::RasterElevationBindingToken token) {
                emit cancelled(jobId, token);
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
std::vector<LoadJobRow> RasterElevationController::jobRows() const
{
    return desktopOperationRows(RasterElevationOperation::jobRows());
}
} // namespace pci
