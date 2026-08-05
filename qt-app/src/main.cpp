#include "videovault/app/main_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/version.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Mega Video Protect"));
    application.setOrganizationName(QStringLiteral("Mega Video Protect"));
    const auto version = videovault::core::version();
    application.setApplicationVersion(QString::fromLatin1(
        version.data(), static_cast<int>(version.size())));
    application.setPalette(videovault::app::darkPalette());
    application.setStyleSheet(videovault::app::appStyleSheet());

    videovault::app::MainWindow window;
    window.show();
    return application.exec();
}
