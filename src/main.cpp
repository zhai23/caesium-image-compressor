#include "MainWindow.h"
#include "ipc/CliClient.h"
#include "ipc/CliRunner.h"
#include "ipc/CliServer.h"
#include "utils/CliPathConfig.h"
#include "utils/Logger.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QJsonArray>
#include <QSettings>
#include <QSharedMemory>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QUuid>

QString loadInstallationId()
{
    if (!QSettings().contains("uuid")) {
        QString newUUID = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSettings().setValue("uuid", newUUID);
        return newUUID;
    } else {
        return QSettings().value("uuid").toString();
    }
}

void loadTheme(QApplication& a)
{
    int themeIndex = QSettings().value("preferences/general/theme", 0).toInt();
    int themeVariantIndex = QSettings().value("preferences/general/theme_variant").toInt();
    if (themeVariantIndex == 1) {
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    } else if (themeVariantIndex == 2) {
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    }

    // From 2.4.1 and Qt 6.5, the framework can handle the switch between dark and light
    if (themeIndex > 1) {
        themeIndex = 1;
    }

#ifdef Q_OS_WIN
    if (themeIndex == 0) {
        a.setStyleSheet("QSplitter::handle:horizontal{image:url(:/icons/ui/v_handle.png);}QSplitter::handle:vertical{image:url(:/icons/ui/h_handle.png);}QStatusBar::item{border: none;}");
    }
#endif

    if (themeIndex == 1) {
        QApplication::setStyle(QStyleFactory::create(THEMES[themeIndex].theme));

        if (QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
            auto palette = QApplication::palette();
            QColor purple(168, 85, 247);
            palette.setColor(QPalette::Highlight, purple);
            if (QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
                QColor darkGray(25, 25, 25);
                // QColor gray(32, 32, 32);
                QColor lightGray(82, 82, 82);
                QColor black(0, 0, 0);
                QColor blue(37, 99, 235);
                QColor white(241, 245, 249);

                palette.setColor(QPalette::Window, darkGray);
                palette.setColor(QPalette::WindowText, white);
                palette.setColor(QPalette::Base, black);
                palette.setColor(QPalette::AlternateBase, darkGray);
                palette.setColor(QPalette::ToolTipBase, blue);
                palette.setColor(QPalette::ToolTipText, white);
                palette.setColor(QPalette::Text, white);
                palette.setColor(QPalette::Button, darkGray);
                palette.setColor(QPalette::ButtonText, white);
                palette.setColor(QPalette::Link, blue);
                palette.setColor(QPalette::HighlightedText, black);

                palette.setColor(QPalette::Active, QPalette::Button, darkGray);
                palette.setColor(QPalette::Disabled, QPalette::ButtonText, lightGray);
                palette.setColor(QPalette::Disabled, QPalette::WindowText, lightGray);
                palette.setColor(QPalette::Disabled, QPalette::Text, lightGray);
                palette.setColor(QPalette::Disabled, QPalette::Light, darkGray);
            }
            QApplication::setPalette(palette);
            QApplication::setStyle(QStyleFactory::create(THEMES[themeIndex].theme));
        }
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication::setOrganizationName("SaeraSoft");
    QCoreApplication::setOrganizationDomain("saerasoft.com");
    QCoreApplication::setApplicationName("Caesium Image Compressor");
    QCoreApplication::setApplicationVersion("2.8.6");

#ifdef NDEBUG
    qInstallMessageHandler(Logger::messageHandler);
#endif

    QApplication a(argc, argv);

    // Refresh the "app-path.txt" pointer used by the caesium-cli.cmd wrapper on
    // PATH. If the application was moved to a new folder, the existing PATH
    // entry keeps working and is never duplicated.
    CliPathConfig::updateAppLocation();

    // Command-line control mode: "caesium --cli <command> ..." connects to the
    // running instance and forwards the command. Everything else is handled by
    // CliRunner, so we bypass the GUI startup entirely.
    QStringList rawArgs = QApplication::arguments();
    if (rawArgs.contains("--cli")) {
        rawArgs.removeFirst(); // program name
        rawArgs.removeAll("--cli");
        return CliRunner::run(rawArgs);
    }

    QCommandLineParser parser;
    parser.addVersionOption();
    parser.process(a);

    // Single-instance gate. On Windows, QLocalServer lets several processes
    // listen on the same named pipe, so QSharedMemory is the authoritative lock:
    // create() only succeeds for the first instance. Any later invocation
    // forwards its file arguments to the running instance and exits.
    QSharedMemory instanceLock("caesium_instance_lock_" + CliProtocol::pipeName());
    if (!instanceLock.create(1)) {
        const QStringList positionalArgs = parser.positionalArguments();
        if (!positionalArgs.isEmpty()) {
            CliClient::sendRequest("import",
                { { "paths", QJsonArray::fromStringList(positionalArgs) } });
        }
        return 0;
    }

    auto* cliServer = new CliServer();
    cliServer->start();

    loadTheme(a);

    qInfo() << "---- Starting application ----";
    QString uuid = loadInstallationId();
    qInfo() << "UUID:" << uuid;
    MainWindow w(cliServer);

    QScreen* spawnScreen = w.screen();
    if (!QGuiApplication::screens().contains(spawnScreen)) {
        QScreen* cursorScreen = QApplication::screenAt(QCursor::pos());
        w.move(cursorScreen->availableGeometry().center() - w.rect().center());
        w.setScreen(cursorScreen);
    } else if (!spawnScreen->availableGeometry().contains(w.pos())) {
        w.move(spawnScreen->availableGeometry().center() - w.rect().center());
    }

    w.show();

    return QApplication::exec();
}
