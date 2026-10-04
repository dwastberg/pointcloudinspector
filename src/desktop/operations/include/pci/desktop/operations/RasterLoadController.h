#pragma once
#include <QObject>
#include <QTimer>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/RasterImportOperation.h>
namespace pci {
class RasterLoadController final
    : public QObject
    , public RasterImportOperation {
    Q_OBJECT
public:
    RasterLoadController(std::shared_ptr<const RasterLoader> loader,
                         TaskScheduler &scheduler,
                         QObject *parent = nullptr);
    ~RasterLoadController() override = default;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
signals:
    // Carries the already-inspected source so the UI thread never reopens it.
    void loaded(pci::LoadJobId jobId,
                pci::RasterLayerDataPtr data,
                bool initiallyVisible);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    QTimer recoveryTimer_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::RasterLayerDataPtr)
