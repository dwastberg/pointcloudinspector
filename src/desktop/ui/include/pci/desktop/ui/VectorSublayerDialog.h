#pragma once

#include <pci/operations/VectorImport.h>

#include <QDialog>

#include <vector>

class QListWidget;
class QLabel;
class QPushButton;

namespace pci {
class VectorSublayerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit VectorSublayerDialog(VectorImportPreflight preflight,
                                  QWidget *parent = nullptr);
    [[nodiscard]] std::vector<VectorSublayerKey> selectedSublayers() const;

private:
    void setAllChecked(bool checked);
    void updateSelectionSummary();

    QListWidget *list_ = nullptr;
    QLabel *selectionSummary_ = nullptr;
    QPushButton *acceptButton_ = nullptr;
    std::vector<VectorSublayerInfo> sublayers_;
};
} // namespace pci
