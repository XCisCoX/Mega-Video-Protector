#include "videovault/app/main_window.hpp"
#include "videovault/core/version.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QPalette>
#include <QString>

namespace {

QPalette darkPalette() {
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

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Mega Video Protect"));
    application.setOrganizationName(QStringLiteral("Mega Video Protect"));
    const auto version = videovault::core::version();
    application.setApplicationVersion(QString::fromLatin1(
        version.data(), static_cast<int>(version.size())));
    application.setPalette(darkPalette());
    application.setStyleSheet(QStringLiteral(R"(
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
    )"));

    videovault::app::MainWindow window;
    window.show();
    return application.exec();
}
