#include "videovault/app/main_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/version.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include <QString>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Mega Vault Protect"));
    application.setOrganizationName(QStringLiteral("Mega Vault Protect"));
    // The previous name stored the vault path and gallery preferences.
    // Copy them once so an existing install still opens the same vault.
    {
        QSettings current;
        QSettings previous(QStringLiteral("Mega Video Protect"), QStringLiteral("Mega Video Protect"));
        const auto keys = previous.allKeys();
        for (const QString& key : keys) {
            if (!current.contains(key)) {
                current.setValue(key, previous.value(key));
            }
        }
    }
    const auto version = videovault::core::version();
    application.setApplicationVersion(QString::fromLatin1(
        version.data(), static_cast<int>(version.size())));
    // Window/taskbar icon on every platform (the .ico resource handles the
    // Explorer icon for the Windows exe itself).
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    QFont appFont(QStringLiteral("Segoe UI"));
    appFont.setPointSize(10);
    application.setFont(appFont);
    application.setPalette(videovault::app::darkPalette());
    application.setStyleSheet(videovault::app::appStyleSheet());

    videovault::app::MainWindow window;
    window.show();
    return application.exec();
}
