#pragma once
#include <QObject>
#include <QTimer>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/RasterElevationOperation.h>
namespace pci {
class RasterElevationController final
    : public QObject
    , public RasterElevationOperation {
    Q_OBJECT
public:
    RasterElevationController(TaskScheduler &scheduler,
                              QObject *parent = nullptr);
    ~RasterElevationController() override = default;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
signals:
    void completed(pci::LoadJobId jobId,
                   pci::RasterElevationBindingToken token,
                   pci::RasterElevationRange range);
    void failed(pci::LoadJobId jobId,
                pci::RasterElevationBindingToken token,
                QString message);
    void cancelled(pci::LoadJobId jobId,
                   pci::RasterElevationBindingToken token);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    QTimer recoveryTimer_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::RasterElevationRange)
Q_DECLARE_METATYPE(pci::RasterElevationBindingToken)
