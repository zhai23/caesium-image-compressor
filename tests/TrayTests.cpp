#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTimer>
#include "MainWindow.h"
#include "dialogs/PreferencesDialog.h"

class TrayTests : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;

    bool eventFilter(QObject* object, QEvent* event) override
    {
        // Exercise the native Windows tray without displaying test windows.
        if (event->type() == QEvent::Show && object->objectName() == "exitPrompt") {
            if (auto* widget = qobject_cast<QWidget*>(object))
                widget->setWindowOpacity(0);
        }
        return false;
    }

    static void showWindow(MainWindow& window)
    {
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
    }

    static QString cacheMarker()
    {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        const QString path = cache + "/tray-session-marker";
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) qFatal("Cannot create isolated cache marker");
        file.write("preserve while running in tray");
        return path;
    }

    static void answerPrompt(QMessageBox::ButtonRole role, bool* answered)
    {
        QTimer::singleShot(0, [role, answered] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* prompt = qobject_cast<QMessageBox*>(widget);
                if (!prompt || prompt->objectName() != "exitPrompt") continue;
                bool hasTrayButton = false;
                for (auto* button : prompt->buttons()) {
                    if (prompt->buttonRole(button) == QMessageBox::ActionRole) {
                        hasTrayButton = true;
                        if (button->isEnabled() != QSystemTrayIcon::isSystemTrayAvailable())
                            qFatal("Incorrect tray button availability");
                    }
                }
                if (!hasTrayButton) qFatal("Missing tray button in exit prompt");
                for (auto* button : prompt->buttons()) {
                    if (prompt->buttonRole(button) == role) {
                        *answered = true;
                        button->click();
                        return;
                    }
                }
            }
            qFatal("Expected exit confirmation dialog");
        });
    }

private slots:
    void initTestCase()
    {
        QVERIFY(settingsDir.isValid());
        QCoreApplication::setOrganizationName("CaesiumTests");
        QCoreApplication::setApplicationName("TrayTests");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        qApp->setQuitOnLastWindowClosed(false);
        qApp->installEventFilter(this);
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.setValue("preferences/general/send_usage_reports", false);
        settings.setValue("preferences/general/check_updates_at_startup", false);
        settings.setValue("preferences/general/prompt_before_exit", false);
        settings.setValue("mainwindow/previews_visible", false);
    }

    void preferencesPersistImmediately()
    {
        MainWindow window;
        PreferencesDialog preferences(&window);
        auto* checkbox = preferences.findChild<QCheckBox*>("closeToTray_CheckBox");
        QVERIFY(checkbox);
        QVERIFY(!checkbox->isChecked());
        checkbox->setChecked(true);
        QVERIFY(QSettings().value("preferences/general/close_to_tray").toBool());
        PreferencesDialog reopened(&window);
        QVERIFY(reopened.findChild<QCheckBox*>("closeToTray_CheckBox")->isChecked());
        checkbox->setChecked(false);
        QVERIFY(!QSettings().value("preferences/general/close_to_tray").toBool());
    }

    void closeWithoutTrayStillExits()
    {
        MainWindow window;
        showWindow(window);
        const QString marker = cacheMarker();
        QVERIFY(window.close());
        QVERIFY(!QFile::exists(marker));
    }

    void closeToTrayPreservesSessionAndRestores()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) QSKIP("Desktop tray is unavailable");
        QSettings().setValue("preferences/general/close_to_tray", true);
        QSettings().setValue("preferences/general/prompt_before_exit", true);
        MainWindow window;
        showWindow(window);
        window.showMaximized();
        const QString marker = cacheMarker();
        QVERIFY(!window.close());
        QVERIFY(window.isHidden());
        QVERIFY(QFile::exists(marker));
        auto* tray = window.findChild<QSystemTrayIcon*>();
        QVERIFY(tray && tray->isVisible());
        QVERIFY(QMetaObject::invokeMethod(tray, "activated", Qt::DirectConnection,
            Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger)));
        QVERIFY(window.isVisible());
        QVERIFY(window.isMaximized());
        QVERIFY(!window.close());
        window.findChild<QAction*>("actionShow")->trigger();
        QVERIFY(window.isVisible());
        QVERIFY(!window.close());
        QVERIFY(window.handleCliRequest("show", {}).ok);
        QVERIFY(window.isVisible());
        QVERIFY(QFile::exists(marker));
    }

    void confirmationCancelTrayAndExit()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) QSKIP("Desktop tray is unavailable");
        QSettings().setValue("preferences/general/prompt_before_exit", true);
        MainWindow window;
        showWindow(window);
        const QString marker = cacheMarker();
        bool answered = false;
        answerPrompt(QMessageBox::RejectRole, &answered);
        QVERIFY(!window.close());
        QVERIFY(answered && window.isVisible());
        answered = false;
        answerPrompt(QMessageBox::ActionRole, &answered);
        QVERIFY(!window.close());
        QVERIFY(answered && window.isHidden());
        QVERIFY(QFile::exists(marker));
        QVERIFY(!QSettings().value("preferences/general/close_to_tray", false).toBool());
        window.findChild<QAction*>("actionShow")->trigger();
        answered = false;
        answerPrompt(QMessageBox::YesRole, &answered);
        QVERIFY(window.close());
        QVERIFY(answered && !QFile::exists(marker));
    }

    void explicitExitAndCancellation()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) QSKIP("Desktop tray is unavailable");
        QSettings().setValue("preferences/general/close_to_tray", true);
        QSettings().setValue("preferences/general/prompt_before_exit", true);
        MainWindow window;
        showWindow(window);
        const QString marker = cacheMarker();
        auto* exitAction = window.findChild<QAction*>("actionExit");
        QVERIFY(exitAction);
        bool answered = false;
        answerPrompt(QMessageBox::RejectRole, &answered);
        exitAction->trigger();
        QVERIFY(answered && window.isVisible());
        // Cancel must reset the explicit-exit flag: closing now hides again.
        QVERIFY(!window.close());
        QVERIFY(window.isHidden() && QFile::exists(marker));
        answered = false;
        answerPrompt(QMessageBox::YesRole, &answered);
        exitAction->trigger();
        QVERIFY(answered && !QFile::exists(marker));
    }

    void unavailableTrayFallsBackToClose()
    {
        if (QSystemTrayIcon::isSystemTrayAvailable()) QSKIP("Run with QT_QPA_PLATFORM=offscreen for this case");
        QSettings().setValue("preferences/general/close_to_tray", true);
        MainWindow window;
        showWindow(window);
        const QString marker = cacheMarker();
        QSettings().setValue("preferences/general/prompt_before_exit", true);
        bool answered = false;
        answerPrompt(QMessageBox::RejectRole, &answered);
        QVERIFY(!window.close());
        QVERIFY(answered && window.isVisible());
        QSettings().setValue("preferences/general/prompt_before_exit", false);
        QVERIFY(window.close());
        QVERIFY(!QFile::exists(marker));
    }

    void cliQuitBypassesTrayAndPrompt()
    {
        QSettings().setValue("preferences/general/close_to_tray", true);
        QSettings().setValue("preferences/general/prompt_before_exit", true);
        MainWindow window;
        showWindow(window);
        const QString marker = cacheMarker();
        const auto response = window.handleCliRequest("quit", {});
        QVERIFY(response.ok);
        QVERIFY(!QFile::exists(marker));
    }

    void nativeEventLoopLifecycle()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) QSKIP("Desktop tray is unavailable");
        for (const QString& probe : {QString("--lifecycle-probe"), QString("--quit-probe")}) {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), {probe});
            QVERIFY(child.waitForFinished(10000));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
        }
    }
};

int main(int argc, char** argv)
{
    // Keep QtTest flags out of MainWindow's application command-line parser.
    int applicationArgc = 1;
    char* applicationArgv[] = {argv[0], nullptr};
    QApplication app(applicationArgc, applicationArgv);
    const bool quitProbe = argc == 2 && QByteArray(argv[1]) == "--quit-probe";
    if (quitProbe || (argc == 2 && QByteArray(argv[1]) == "--lifecycle-probe")) {
        QTemporaryDir settingsDir;
        QCoreApplication::setOrganizationName("CaesiumTests");
        QCoreApplication::setApplicationName("TrayLifecycleTests");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().setValue("preferences/general/send_usage_reports", false);
        QSettings().setValue("preferences/general/check_updates_at_startup", false);
        QSettings().setValue("preferences/general/close_to_tray", true);
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QTimer::singleShot(0, [&] {
            if (window.close() || !window.isHidden()) app.exit(10);
        });
        QTimer::singleShot(150, [&] {
            // Closing the last window to the tray must leave the event loop alive.
            window.findChild<QAction*>("actionShow")->trigger();
            if (!window.isVisible() || (!quitProbe && window.close())) app.exit(11);
        });
        QTimer::singleShot(300, [&] {
            if (quitProbe) app.quit();
            else window.findChild<QAction*>("actionExit")->trigger();
        });
        QTimer::singleShot(2000, [&] { app.exit(12); });
        return app.exec();
    }
    TrayTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "TrayTests.moc"
