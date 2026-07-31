#include "videovault/core/version.hpp"

#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QPalette>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(14, 17, 23));
    palette.setColor(QPalette::WindowText, QColor(235, 239, 245));
    palette.setColor(QPalette::Base, QColor(20, 24, 32));
    palette.setColor(QPalette::AlternateBase, QColor(27, 32, 42));
    palette.setColor(QPalette::Text, QColor(235, 239, 245));
    palette.setColor(QPalette::Button, QColor(35, 42, 55));
    palette.setColor(QPalette::ButtonText, QColor(235, 239, 245));
    palette.setColor(QPalette::Highlight, QColor(91, 124, 250));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    return palette;
}

QLabel* makeLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Mega Video Protect"));
    app.setOrganizationName(QStringLiteral("Mega Video Protect"));
    app.setApplicationVersion(QString::fromLatin1(videovault::core::version().data()));
    app.setPalette(darkPalette());
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #0e1117; }
        QFrame#card {
            background: #171c25;
            border: 1px solid #2b3342;
            border-radius: 16px;
        }
        QLabel#title { color: #f1f4f8; font-size: 28px; font-weight: 600; }
        QLabel#subtitle { color: #9aa7b8; font-size: 14px; }
        QLabel#status { color: #7dd3a7; font-size: 13px; }
    )"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Mega Video Protect"));
    window.resize(980, 640);
    window.setMinimumSize(760, 480);

    auto* root = new QWidget(&window);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(72, 72, 72, 72);
    rootLayout->addStretch();

    auto* card = new QFrame(root);
    card->setObjectName(QStringLiteral("card"));
    card->setMaximumWidth(640);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(48, 44, 48, 44);
    cardLayout->setSpacing(16);
    cardLayout->addWidget(makeLabel(QStringLiteral("Mega Video Protect"), QStringLiteral("title"), card));
    cardLayout->addWidget(makeLabel(
        QStringLiteral("Secure video vault foundation"), QStringLiteral("subtitle"), card));
    cardLayout->addSpacing(12);
    cardLayout->addWidget(makeLabel(
        QStringLiteral("Phase 1 toolchain and module integration is ready."),
        QStringLiteral("status"), card));
    cardLayout->addWidget(makeLabel(
        QStringLiteral("Vault setup remains disabled until the audited crypto and SQLCipher dependencies are integrated."),
        QStringLiteral("subtitle"), card));

    rootLayout->addWidget(card, 0, Qt::AlignHCenter);
    rootLayout->addStretch();
    window.setCentralWidget(root);
    window.show();

    return app.exec();
}
