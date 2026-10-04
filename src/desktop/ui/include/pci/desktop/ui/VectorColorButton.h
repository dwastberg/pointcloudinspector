#pragma once

#include <pci/vector/VectorLayerStyle.h>

#include <QPushButton>

namespace pci {
class VectorColorButton final : public QPushButton {
    Q_OBJECT
public:
    explicit VectorColorButton(QWidget *parent = nullptr);
    [[nodiscard]] VectorRgba color() const noexcept;
    void setColor(VectorRgba color);
signals:
    void colorChanged(pci::VectorRgba color);

private:
    void refreshAppearance();
    VectorRgba color_;
};
} // namespace pci
Q_DECLARE_METATYPE(pci::VectorRgba)
