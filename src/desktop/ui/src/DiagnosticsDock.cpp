#include <pci/desktop/ui/DiagnosticsDock.h>

#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace pci {

DiagnosticsDock::DiagnosticsDock(QWidget *parent)
    : QDockWidget(QStringLiteral("Diagnostics"), parent)
{
    setObjectName(QStringLiteral("pointCloudDiagnosticsPanel"));
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    auto *group = new QGroupBox(QStringLiteral("Renderer and residency"), this);
    group->setObjectName(QStringLiteral("pointCloudResidencyDiagnostics"));
    auto *layout = new QVBoxLayout(group);
    value_ = new QLabel(QStringLiteral("No renderer metrics yet."), group);
    value_->setObjectName(
        QStringLiteral("pointCloudResidencyDiagnosticsValue"));
    value_->setWordWrap(true);
    value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(value_);
    setWidget(group);
    hide();
}

void DiagnosticsDock::setDiagnostics(const QString &diagnostics)
{
    value_->setText(diagnostics);
    value_->setToolTip(diagnostics);
}

} // namespace pci
