#include "app/StrataTheme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

namespace pci {

void applyStrataTheme(QApplication &application)
{
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#191d22")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#eef1f5")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#111419")));
    palette.setColor(QPalette::AlternateBase,
                     QColor(QStringLiteral("#171b20")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#282e36")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f4f6f8")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#e6e9ed")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#252b32")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#eef1f5")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#74a8ff")));
    palette.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#e87878")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2f6fb3")));
    palette.setColor(QPalette::HighlightedText,
                     QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::PlaceholderText,
                     QColor(QStringLiteral("#7f8791")));

    palette.setColor(QPalette::Disabled,
                     QPalette::WindowText,
                     QColor(QStringLiteral("#858d97")));
    palette.setColor(
        QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#747c86")));
    palette.setColor(QPalette::Disabled,
                     QPalette::ButtonText,
                     QColor(QStringLiteral("#747c86")));
    application.setPalette(palette);

    application.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog, QMessageBox {
            background: #191d22;
        }
        QMenuBar {
            background: #191d22;
            border-bottom: 1px solid #343b44;
            spacing: 4px;
        }
        QMenuBar::item {
            padding: 5px 8px;
            border-radius: 3px;
        }
        QMenuBar::item:selected, QMenu::item:selected {
            background: #2f6fb3;
            color: white;
        }
        QMenu {
            background: #22272e;
            border: 1px solid #3b424c;
            padding: 5px;
        }
        QMenu::item {
            padding: 6px 28px 6px 9px;
            border-radius: 3px;
        }
        QToolBar {
            background: #20252b;
            border: 0;
            border-bottom: 1px solid #343b44;
            spacing: 6px;
            padding: 4px 8px;
        }
        QToolBar#viewportToolRail {
            border-right: 1px solid #343b44;
            border-bottom: 0;
            padding: 6px 4px;
        }
        QToolButton {
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 5px 7px;
        }
        QToolButton:hover {
            background: #2a3038;
            border-color: #414a55;
        }
        QToolButton:checked {
            background: #244f7e;
            border-color: #4c8dff;
        }
        QDockWidget {
            color: #eef1f5;
            font-weight: 600;
            titlebar-close-icon: none;
            titlebar-normal-icon: none;
        }
        QDockWidget::title {
            background: #20252b;
            border-bottom: 1px solid #343b44;
            padding: 8px 10px;
            text-align: left;
        }
        QListWidget {
            background: #14181d;
            border: 1px solid #303741;
            border-radius: 4px;
            outline: 0;
        }
        QListWidget::item {
            min-height: 28px;
            padding: 2px 6px;
            border-bottom: 1px solid #20262d;
        }
        QListWidget::item:selected {
            background: #244f7e;
            color: white;
        }
        QListWidget::item:hover:!selected {
            background: #20262d;
        }
        QGroupBox {
            border: 0;
            border-top: 1px solid #343b44;
            margin-top: 12px;
            padding-top: 12px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 0;
            padding: 0 6px 0 0;
            color: #f2f4f7;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            min-height: 26px;
            background: #111419;
            border: 1px solid #3b444f;
            border-radius: 4px;
            padding: 1px 6px;
            selection-background-color: #2f6fb3;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus,
        QDoubleSpinBox:focus, QListWidget:focus {
            border-color: #4c8dff;
        }
        QPushButton {
            min-height: 26px;
            background: #252b32;
            border: 1px solid #414a55;
            border-radius: 4px;
            padding: 2px 9px;
        }
        QPushButton:hover {
            background: #2e353e;
            border-color: #566272;
        }
        QPushButton:pressed {
            background: #20262d;
        }
        QPushButton:disabled {
            background: #1d2228;
            border-color: #303741;
        }
        QPushButton[primary="true"] {
            background: #2f6fb3;
            border-color: #4c8dff;
            color: #ffffff;
            font-weight: 600;
        }
        QPushButton[primary="true"]:hover {
            background: #397cc4;
            border-color: #74a8ff;
        }
        QLabel[notice="info"], QLabel[notice="warning"],
        QLabel[notice="error"] {
            padding: 8px;
            border-radius: 4px;
        }
        QLabel[notice="info"] {
            background: #1c2b39;
            border: 1px solid #355979;
            color: #bfd9f2;
        }
        QLabel[notice="warning"] {
            background: #332b1c;
            border: 1px solid #6f5826;
            color: #f0d79a;
        }
        QLabel[notice="error"] {
            background: #392124;
            border: 1px solid #7a3d43;
            color: #f3bdc1;
        }
        QCheckBox {
            spacing: 6px;
        }
        QScrollArea {
            background: #191d22;
        }
        QStatusBar {
            background: #20252b;
            border-top: 1px solid #343b44;
            color: #b8bec7;
        }
        QProgressBar {
            border: 1px solid #414a55;
            border-radius: 4px;
            background: #111419;
            text-align: center;
        }
        QProgressBar::chunk {
            background: #4c8dff;
            border-radius: 3px;
        }
        QProgressBar#loadTaskProgressBar {
            min-height: 4px;
            max-height: 4px;
            border: 0;
            border-radius: 2px;
        }
        QProgressBar#loadTaskProgressBar[state="attention"]::chunk {
            background: #d0a34a;
        }
        QLabel#loadTaskDetail,
        QLabel#vectorImportSource,
        QLabel#vectorImportInstructions,
        QLabel#vectorPlacementNotice {
            color: #aab1ba;
        }
        QLabel#vectorImportHeader {
            color: #f4f6f8;
        }
        QLabel#vectorSublayerSummary {
            color: #d6dbe1;
            font-weight: 600;
        }
        QWidget#vectorExtentWarning {
            background: #332b1c;
            border: 1px solid #6f5826;
            border-radius: 4px;
        }
        QWidget#vectorExtentWarning QLabel {
            color: #f0d79a;
        }
        QWidget#vectorExtentWarning QPushButton {
            background: #3e3422;
            border-color: #80672e;
        }
        QSplitter::handle {
            background: #343b44;
        }
    )"));
}

} // namespace pci
