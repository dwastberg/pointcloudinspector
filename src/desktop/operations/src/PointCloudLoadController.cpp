#include "OperationRows.h"
#include <pci/adapters/platform/ProcessMemory.h>
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/operations/PointCloudLoadController.h>
namespace pci {
PointCloudLoadController::PointCloudLoadController(
    std::shared_ptr<const PointCloudLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , PointImportOperation(loader, scheduler, makeQtCompletionExecutor(this))
{
    qRegisterMetaType<PreparedPointDatasetPtr>();
    qRegisterMetaType<PointDatasetEvent>();
    qRegisterMetaType<PointCloudImportStage>();
    qRegisterMetaType<LoadJobId>();
    setEvents({
        .progressChanged =
            [this](pci::LoadJobId jobId,
                   pci::PointCloudImportStage stage,
                   std::uint64_t processed,
                   std::uint64_t total) {
                emit progressChanged(jobId, stage, processed, total);
            },
        .dataReady =
            [this](pci::LoadJobId jobId, pci::PointDatasetEvent event) {
                emit dataReady(jobId, event);
            },
        .prepared =
            [this](pci::LoadJobId jobId, pci::PreparedPointDatasetPtr dataset) {
                emit prepared(jobId, dataset);
            },
        .loaded =
            [this](pci::LoadJobId jobId, pci::PreparedPointDatasetPtr dataset) {
                emit loaded(jobId, dataset);
            },
        .failed =
            [this](pci::LoadJobId jobId, std::string message) {
                emit failed(jobId, QString::fromStdString(message));
            },
        .cancelled =
            [this](pci::LoadJobId jobId) {
                emit cancelled(jobId);
            },
        .schedulingChanged =
            [this]() {
                emit schedulingChanged();
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
std::vector<LoadJobRow> PointCloudLoadController::jobRows() const
{
    return desktopOperationRows(PointImportOperation::jobRows());
}
PointCloudLoadControllerMetrics PointCloudLoadController::metrics() const
{
    auto result = PointImportOperation::operationMetrics();
    const auto memory = processMemoryMetrics();
    result.processResidentBytes = memory.residentBytes;
    result.peakProcessResidentBytes = memory.peakResidentBytes;
    return result;
}
} // namespace pci
