#include "OperationRows.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/operations/VectorLoadController.h>
namespace pci {
VectorLoadController::VectorLoadController(
    std::shared_ptr<const VectorLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , VectorImportOperation(loader, scheduler, makeQtCompletionExecutor(this))
{
    qRegisterMetaType<VectorLayerDataPtr>();
    qRegisterMetaType<VectorSublayerKey>();
    qRegisterMetaType<VectorLoadSummary>();
    qRegisterMetaType<VectorImportPreflight>();
    qRegisterMetaType<LoadJobId>();
    setEvents({
        .inspected =
            [this](pci::LoadJobId jobId, pci::VectorImportPreflight preflight) {
                emit inspected(jobId, preflight);
            },
        .progressChanged =
            [this](pci::LoadJobId jobId,
                   std::uint64_t processed,
                   std::uint64_t total) {
                emit progressChanged(jobId, processed, total);
            },
        .sublayerLoaded =
            [this](pci::LoadJobId jobId,
                   pci::VectorSublayerKey key,
                   pci::VectorLayerDataPtr data) {
                emit sublayerLoaded(jobId, key, data);
            },
        .sublayerFailed =
            [this](pci::LoadJobId jobId,
                   pci::VectorSublayerKey key,
                   std::string message) {
                emit sublayerFailed(
                    jobId, key, QString::fromStdString(message));
            },
        .loaded =
            [this](pci::LoadJobId jobId, pci::VectorLoadSummary summary) {
                emit loaded(jobId, summary);
            },
        .finished =
            [this](pci::LoadJobId jobId, pci::VectorLoadSummary summary) {
                emit finished(jobId, summary);
            },
        .failed =
            [this](pci::LoadJobId jobId, std::string message) {
                emit failed(jobId, QString::fromStdString(message));
            },
        .cancelled =
            [this](pci::LoadJobId jobId, pci::VectorLoadSummary summary) {
                emit cancelled(jobId, summary);
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
std::vector<LoadJobRow> VectorLoadController::jobRows() const
{
    return desktopOperationRows(VectorImportOperation::jobRows());
}
} // namespace pci
