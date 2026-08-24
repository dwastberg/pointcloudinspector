#include "app/ToolbarIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

#include <array>

namespace pci {
namespace {

constexpr int iconExtent = 20;

void drawNode(QPainter &painter, const QPointF center, const QColor &color)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(center, 1.8, 1.8);
    painter.restore();
}

void drawGlyph(QPainter &painter,
               const ToolbarIcon icon,
               const QColor &foreground,
               const QColor &accent)
{
    QPen pen(foreground, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (icon) {
    case ToolbarIcon::Open: {
        QPainterPath folder;
        folder.moveTo(2.5, 6.0);
        folder.lineTo(7.5, 6.0);
        folder.lineTo(9.2, 8.0);
        folder.lineTo(17.5, 8.0);
        folder.lineTo(16.0, 16.0);
        folder.lineTo(3.5, 16.0);
        folder.closeSubpath();
        painter.drawPath(folder);
        painter.drawLine(QPointF(3.0, 6.0), QPointF(3.0, 14.0));
        break;
    }
    case ToolbarIcon::Vector: {
        const std::array points{QPointF(2.8, 15.2),
                                QPointF(7.5, 6.0),
                                QPointF(12.5, 12.0),
                                QPointF(17.2, 4.2)};
        painter.drawPolyline(points.data(), static_cast<int>(points.size()));
        for (const QPointF point : points) {
            drawNode(painter, point, accent);
        }
        break;
    }
    case ToolbarIcon::Raster: {
        // A framed grid: imagery is a pixel field, not a feature set.
        painter.drawRect(QRectF(3.0, 4.0, 14.0, 12.0));
        painter.drawLine(QPointF(7.7, 4.0), QPointF(7.7, 16.0));
        painter.drawLine(QPointF(12.3, 4.0), QPointF(12.3, 16.0));
        painter.drawLine(QPointF(3.0, 8.0), QPointF(17.0, 8.0));
        painter.drawLine(QPointF(3.0, 12.0), QPointF(17.0, 12.0));
        drawNode(painter, QPointF(5.35, 10.0), accent);
        drawNode(painter, QPointF(14.65, 14.0), accent);
        break;
    }
    case ToolbarIcon::ColorizeRaster: {
        painter.drawRect(QRectF(2.5, 3.0, 8.0, 8.0));
        painter.drawLine(QPointF(6.5, 3.0), QPointF(6.5, 11.0));
        painter.drawLine(QPointF(2.5, 7.0), QPointF(10.5, 7.0));
        painter.drawLine(QPointF(9.5, 10.0), QPointF(14.0, 14.0));
        painter.drawLine(QPointF(11.5, 14.0), QPointF(14.0, 14.0));
        painter.drawLine(QPointF(14.0, 14.0), QPointF(14.0, 11.5));
        drawNode(painter, QPointF(16.8, 8.0), accent);
        drawNode(painter, QPointF(17.0, 14.0), accent);
        drawNode(painter, QPointF(12.0, 17.0), accent);
        break;
    }
    case ToolbarIcon::RevertColors: {
        QPainterPath arrow;
        arrow.moveTo(5.0, 7.0);
        arrow.lineTo(2.7, 9.5);
        arrow.lineTo(5.5, 11.5);
        arrow.moveTo(3.0, 9.5);
        arrow.cubicTo(5.0, 4.0, 14.5, 4.0, 16.5, 10.0);
        arrow.cubicTo(17.5, 13.0, 15.5, 16.0, 12.0, 16.5);
        painter.drawPath(arrow);
        drawNode(painter, QPointF(9.0, 10.0), accent);
        drawNode(painter, QPointF(12.5, 12.0), accent);
        break;
    }
    case ToolbarIcon::Fit:
        painter.drawLine(QPointF(3.0, 7.0), QPointF(3.0, 3.0));
        painter.drawLine(QPointF(3.0, 3.0), QPointF(7.0, 3.0));
        painter.drawLine(QPointF(13.0, 3.0), QPointF(17.0, 3.0));
        painter.drawLine(QPointF(17.0, 3.0), QPointF(17.0, 7.0));
        painter.drawLine(QPointF(17.0, 13.0), QPointF(17.0, 17.0));
        painter.drawLine(QPointF(17.0, 17.0), QPointF(13.0, 17.0));
        painter.drawLine(QPointF(7.0, 17.0), QPointF(3.0, 17.0));
        painter.drawLine(QPointF(3.0, 17.0), QPointF(3.0, 13.0));
        drawNode(painter, QPointF(7.0, 11.8), accent);
        drawNode(painter, QPointF(10.2, 7.2), accent);
        drawNode(painter, QPointF(13.4, 12.2), accent);
        break;
    case ToolbarIcon::TopDown:
        painter.drawLine(QPointF(10.0, 2.5), QPointF(10.0, 11.0));
        painter.drawLine(QPointF(6.8, 8.0), QPointF(10.0, 11.2));
        painter.drawLine(QPointF(13.2, 8.0), QPointF(10.0, 11.2));
        painter.drawLine(QPointF(3.0, 15.5), QPointF(17.0, 15.5));
        painter.drawLine(QPointF(5.0, 13.0), QPointF(15.0, 13.0));
        break;
    case ToolbarIcon::Orthographic:
        painter.drawRect(QRectF(3.0, 5.0, 10.0, 11.0));
        painter.drawLine(QPointF(6.0, 2.5), QPointF(16.5, 2.5));
        painter.drawLine(QPointF(16.5, 2.5), QPointF(16.5, 13.5));
        painter.drawLine(QPointF(13.0, 5.0), QPointF(16.5, 2.5));
        painter.drawLine(QPointF(13.0, 16.0), QPointF(16.5, 13.5));
        break;
    case ToolbarIcon::Settings:
        painter.drawLine(QPointF(2.5, 5.0), QPointF(17.5, 5.0));
        painter.drawLine(QPointF(2.5, 10.0), QPointF(17.5, 10.0));
        painter.drawLine(QPointF(2.5, 15.0), QPointF(17.5, 15.0));
        drawNode(painter, QPointF(7.0, 5.0), accent);
        drawNode(painter, QPointF(13.0, 10.0), accent);
        drawNode(painter, QPointF(8.5, 15.0), accent);
        break;
    case ToolbarIcon::Navigate: {
        QPainterPath cursor;
        cursor.moveTo(3.0, 2.5);
        cursor.lineTo(15.2, 10.0);
        cursor.lineTo(9.5, 11.0);
        cursor.lineTo(12.8, 16.7);
        cursor.lineTo(10.2, 18.0);
        cursor.lineTo(7.0, 12.2);
        cursor.lineTo(3.0, 16.0);
        cursor.closeSubpath();
        painter.drawPath(cursor);
        break;
    }
    case ToolbarIcon::Measure:
        painter.drawLine(QPointF(4.0, 15.5), QPointF(16.0, 4.5));
        painter.drawLine(QPointF(2.5, 13.8), QPointF(5.5, 17.2));
        painter.drawLine(QPointF(14.5, 2.8), QPointF(17.5, 6.2));
        drawNode(painter, QPointF(4.0, 15.5), accent);
        drawNode(painter, QPointF(16.0, 4.5), accent);
        break;
    }
}

QPixmap renderGlyph(const ToolbarIcon icon,
                    const QPalette &palette,
                    const qreal devicePixelRatio)
{
    QPixmap pixmap(qRound(iconExtent * devicePixelRatio),
                   qRound(iconExtent * devicePixelRatio));
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    drawGlyph(painter,
              icon,
              palette.color(QPalette::ButtonText),
              palette.color(QPalette::Highlight));
    return pixmap;
}

} // namespace

QIcon toolbarIcon(const ToolbarIcon icon, const QPalette &palette)
{
    QIcon result;
    result.addPixmap(renderGlyph(icon, palette, 1.0));
    result.addPixmap(renderGlyph(icon, palette, 2.0));
    return result;
}

} // namespace pci
