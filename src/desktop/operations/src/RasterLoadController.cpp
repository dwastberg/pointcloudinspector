#include "OperationRows.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/operations/RasterLoadController.h>
namespace pci {
RasterLoadController::RasterLoadController(
    std::shared_ptr<const RasterLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , RasterImportOperation(loader, scheduler, makeQtCompletionExecutor(this))
{
    qRegisterMetaType<RasterLayerDataPtr>();
    qRegisterMetaType<LoadJobId>();
    setEvents({
        .loaded =
            [this](pci::LoadJobId jobId,
                   pci::RasterLayerDataPtr data,
                   bool initiallyVisible) {
                emit loaded(jobId, data, initiallyVisible);
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
std::vector<LoadJobRow> RasterLoadController::jobRows() const
{
    return desktopOperationRows(RasterImportOperation::jobRows());
}
} // namespace pci
