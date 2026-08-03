#pragma once

#include <QDockWidget>

class QLabel;

namespace pci {

class DiagnosticsDock final : public QDockWidget {
public:
    explicit DiagnosticsDock(QWidget *parent = nullptr);

    void setDiagnostics(const QString &diagnostics);

private:
    QLabel *value_ = nullptr;
};

} // namespace pci
