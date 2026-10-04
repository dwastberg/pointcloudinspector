#pragma once
#include <QObject>
#include <QTimer>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/VectorImportOperation.h>
namespace pci {
class VectorLoadController final
    : public QObject
    , public VectorImportOperation {
    Q_OBJECT
public:
    VectorLoadController(std::shared_ptr<const VectorLoader> loader,
                         TaskScheduler &scheduler,
                         QObject *parent = nullptr);
    ~VectorLoadController() override = default;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
signals:
    void inspected(pci::LoadJobId jobId, pci::VectorImportPreflight preflight);
    void
    progressChanged(pci::LoadJobId jobId, quint64 processed, quint64 total);
    void sublayerLoaded(pci::LoadJobId jobId,
                        pci::VectorSublayerKey key,
                        pci::VectorLayerDataPtr data);
    void sublayerFailed(pci::LoadJobId jobId,
                        pci::VectorSublayerKey key,
                        QString message);
    void loaded(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void finished(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    QTimer recoveryTimer_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::VectorLayerDataPtr)
Q_DECLARE_METATYPE(pci::VectorSublayerKey)
Q_DECLARE_METATYPE(pci::VectorLoadSummary)
Q_DECLARE_METATYPE(pci::VectorImportPreflight)
