#pragma once

// Shared application theme: the dark palette and global stylesheet used by
// main() and by tooling that renders the real UI (e.g. the screenshot
// harness). Kept in one place so every entry point renders identically.

#include <QColor>
#include <QPalette>
#include <QString>

namespace videovault::app {

inline QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(12, 15, 21));
    palette.setColor(QPalette::WindowText, QColor(238, 242, 248));
    palette.setColor(QPalette::Base, QColor(20, 24, 32));
    palette.setColor(QPalette::AlternateBase, QColor(27, 32, 42));
    palette.setColor(QPalette::Text, QColor(238, 242, 248));
    palette.setColor(QPalette::PlaceholderText, QColor(112, 124, 142));
    palette.setColor(QPalette::Button, QColor(35, 42, 55));
    palette.setColor(QPalette::ButtonText, QColor(238, 242, 248));
    palette.setColor(QPalette::Highlight, QColor(91, 124, 250));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    return palette;
}

inline QString appStyleSheet() {
    return QStringLiteral(R"(
        QMainWindow, QStackedWidget { background: #0c0f15; }
        QFrame#card {
            background: #171c25;
            border: 1px solid #2b3444;
            border-radius: 16px;
        }
        QLabel#pageTitle {
            color: #f3f6fa;
            font-size: 25px;
            font-weight: 600;
        }
        QLabel#description { color: #9eabba; font-size: 13px; }
        QLabel#errorLabel {
            color: #ff9a9a;
            background: #321d24;
            border: 1px solid #6a303a;
            border-radius: 7px;
            padding: 9px;
        }
        QLineEdit, QComboBox {
            min-height: 39px;
            color: #edf1f6;
            background: #10151d;
            border: 1px solid #364154;
            border-radius: 8px;
            padding: 0 11px;
            selection-background-color: #5b7cfa;
        }
        QLineEdit:focus, QComboBox:focus { border: 1px solid #6f8cff; }
        QPushButton {
            min-height: 39px;
            color: #e9edf4;
            background: #252d3b;
            border: 1px solid #3a465a;
            border-radius: 8px;
            padding: 0 16px;
        }
        QPushButton:hover { background: #303a4b; }
        QPushButton:disabled { color: #697587; background: #1d232d; }
        QPushButton[primary="true"] {
            color: white;
            background: #536fe8;
            border-color: #6f88f4;
            font-weight: 600;
        }
        QPushButton[primary="true"]:hover { background: #627cf0; }
        QDialog { background: #0c0f15; }
        QTreeWidget#gallery, QListWidget#gallery {
            background: #10151d;
            alternate-background-color: #161c27;
            color: #e2e7ef;
            border: none;
            outline: none;
        }
        QTreeWidget#gallery::item, QListWidget#gallery::item {
            color: #e2e7ef;
            padding: 4px;
        }
        QTreeWidget#gallery::item:selected, QListWidget#gallery::item:selected {
            background: #2b3a63;
            color: #ffffff;
        }
        QTreeWidget#gallery::item:hover, QListWidget#gallery::item:hover {
            background: #232c3d;
        }
        QHeaderView { background: #1a202c; color: #aab6c8; }
        QHeaderView::section {
            background: #1a202c;
            color: #aab6c8;
            border: none;
            border-right: 1px solid #2b3444;
            border-bottom: 1px solid #2b3444;
            padding: 6px 8px;
        }
        QSlider::groove:horizontal {
            height: 5px;
            background: #2b3444;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #536fe8;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px;
            height: 14px;
            margin: -5px 0;
            border-radius: 7px;
            background: #7f97f5;
            border: 1px solid #9db1f8;
        }
        QSlider::handle:horizontal:hover { background: #93a8f7; }
        QScrollBar:vertical { background: #10151d; width: 12px; }
        QScrollBar::handle:vertical {
            background: #3a465a;
            border-radius: 5px;
            min-height: 30px;
            margin: 2px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: #10151d; height: 12px; }
        QScrollBar::handle:horizontal {
            background: #3a465a;
            border-radius: 5px;
            min-width: 30px;
            margin: 2px;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");
}

} // namespace videovault::app
