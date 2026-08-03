#include "app/VectorColorButton.h"

#include <QColorDialog>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

float bounded(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

QIcon colorSwatch(const QColor &color)
{
    QPixmap pixmap(24, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    constexpr int tile = 4;
    for (int y = 1; y < 15; y += tile) {
        for (int x = 1; x < 23; x += tile) {
            painter.fillRect(x,
                             y,
                             tile,
                             tile,
                             ((x / tile) + (y / tile)) % 2 == 0
                                 ? QColor(QStringLiteral("#d9dde2"))
                                 : QColor(QStringLiteral("#737b85")));
        }
    }
    painter.fillRect(QRect(1, 1, 22, 14), color);
    painter.setPen(QColor(QStringLiteral("#59616b")));
    painter.drawRoundedRect(QRect(0, 0, 23, 15), 3, 3);
    return QIcon(pixmap);
}

} // namespace

VectorColorButton::VectorColorButton(QWidget *parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("vectorColorButton"));
    setIconSize(QSize(24, 16));
    connect(this, &QPushButton::clicked, this, [this] {
        const QColor selected = QColorDialog::getColor(
            QColor::fromRgbF(
                color_.red, color_.green, color_.blue, color_.alpha),
            this,
            tr("Vector color"),
            QColorDialog::ShowAlphaChannel);
        if (selected.isValid())
            setColor({selected.redF(),
                      selected.greenF(),
                      selected.blueF(),
                      selected.alphaF()});
    });
    refreshAppearance();
}
VectorRgba VectorColorButton::color() const noexcept
{
    return color_;
}
void VectorColorButton::setColor(VectorRgba color)
{
    color = {bounded(color.red),
             bounded(color.green),
             bounded(color.blue),
             bounded(color.alpha)};
    if (color == color_)
        return;
    color_ = color;
    refreshAppearance();
    emit colorChanged(color_);
}
void VectorColorButton::refreshAppearance()
{
    const QColor swatch =
        QColor::fromRgbF(color_.red, color_.green, color_.blue, color_.alpha);
    setIcon(colorSwatch(swatch));
    const int alphaPercent = qRound(swatch.alphaF() * 100.0);
    setText(alphaPercent == 100
                ? swatch.name(QColor::HexRgb).toUpper()
                : tr("%1 · %2%")
                      .arg(swatch.name(QColor::HexRgb).toUpper())
                      .arg(alphaPercent));
    setAccessibleDescription(tr("Vector color %1, %2 percent alpha")
                                 .arg(swatch.name(QColor::HexRgb))
                                 .arg(alphaPercent));
    setToolTip(tr("Choose color and alpha"));
}
} // namespace pci
