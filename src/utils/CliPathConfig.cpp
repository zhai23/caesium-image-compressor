#include "CliPathConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace CliPathConfig {

bool isSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

// Dedicated single-file bin folder that is actually added to PATH, so the whole
// application folder never has to be exposed to the environment.
QString binDir()
{
#ifdef Q_OS_WIN
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        return QDir::toNativeSeparators(
            QDir(localAppData).filePath(QStringLiteral("CaesiumImageCompressor/bin")));
    }
#endif
    return QDir::toNativeSeparators(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("bin")));
}

QString cliFolder()
{
    return binDir();
}

QString cliWrapperPath()
{
    return QDir(binDir()).filePath(QStringLiteral("caesium-cli.cmd"));
}

static QString appPathFile()
{
    return QDir(binDir()).filePath(QStringLiteral("app-path.txt"));
}

static QString readAppPath()
{
    QFile file(appPathFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    const QString path = QString::fromUtf8(file.readAll()).trimmed();
    file.close();
    return path;
}

QString currentCliPath()
{
    const QString recorded = readAppPath();
    if (!recorded.isEmpty() && QFile::exists(recorded)) {
        return QDir::toNativeSeparators(recorded);
    }
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

bool updateAppLocation()
{
#ifdef Q_OS_WIN
    if (!QDir().mkpath(binDir())) {
        return false;
    }
    QFile file(appPathFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toUtf8());
    file.close();
    return true;
#else
    return false;
#endif
}

static QString userPathValue()
{
#ifdef Q_OS_WIN
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return QString();
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (result == ERROR_SUCCESS && size > 0) {
        QByteArray buffer(static_cast<int>(size), '\0');
        result = RegQueryValueExW(key, L"Path", nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()), &size);
        if (result == ERROR_SUCCESS) {
            const QString value = QString::fromUtf16(
                reinterpret_cast<const char16_t*>(buffer.constData()),
                static_cast<int>(size / 2));
            // RegQueryValueEx does not guarantee a NUL terminator; cut at the first one.
            const int nul = value.indexOf(QChar('\0'));
            RegCloseKey(key);
            return nul >= 0 ? value.left(nul) : value;
        }
    }
    RegCloseKey(key);
#endif
    return QString();
}

static bool setUserPathValue(const QString& value)
{
#ifdef Q_OS_WIN
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring data = value.toStdWString();
    // Keep REG_EXPAND_SZ so %VAR% entries keep expanding.
    const LONG result = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
        reinterpret_cast<const BYTE*>(data.c_str()),
        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result == ERROR_SUCCESS) {
        // Notify Explorer and future terminals that the environment changed.
        // Short timeout: a hung window must not block the GUI thread for long.
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 1000, nullptr);
        return true;
    }
#else
    Q_UNUSED(value);
#endif
    return false;
}

// Writes a minimal "caesium-cli.cmd" wrapper into the dedicated bin folder if it
// is missing. The wrapper resolves the real executable location from the
// "app-path.txt" pointer file (refreshed on every startup and on every add), so
// the PATH entry stays valid even if the application is moved to a new folder.
static bool ensureWrapper()
{
    if (!QDir().mkpath(binDir())) {
        return false;
    }
    if (!updateAppLocation()) {
        return false;
    }
    const QString path = cliWrapperPath();
    if (QFile::exists(path)) {
        return true;
    }
    const QString content = QStringLiteral(
        "@echo off\r\n"
        "rem Caesium Image Compressor - command line control\r\n"
        "setlocal\r\n"
        "set \"BINDIR=%~dp0\"\r\n"
        "set \"APPPATH=\"\r\n"
        "if exist \"%BINDIR%app-path.txt\" (\r\n"
        "    set /p APPPATH=<\"%BINDIR%app-path.txt\"\r\n"
        ")\r\n"
        "if not defined APPPATH (\r\n"
        "    set \"APPPATH=%BINDIR%Caesium Image Compressor.exe\"\r\n"
        ")\r\n"
        "if not exist \"%APPPATH%\" (\r\n"
        "    echo Error: could not find \"Caesium Image Compressor.exe\" at \"%APPPATH%\" 1>&2\r\n"
        "    echo Start the application once so the PATH entry is refreshed. 1>&2\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "set \"OUTFILE=%TEMP%\\caesium-cli-%RANDOM%-%RANDOM%.out\"\r\n"
        "\"%APPPATH%\" --cli %* > \"%OUTFILE%\" 2>&1\r\n"
        "set \"EXIT_CODE=%ERRORLEVEL%\"\r\n"
        "if exist \"%OUTFILE%\" (\r\n"
        "    type \"%OUTFILE%\"\r\n"
        "    del \"%OUTFILE%\" >nul 2>&1\r\n"
        ")\r\n"
        "exit /b %EXIT_CODE%\r\n");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(content.toUtf8());
    file.close();
    return true;
}

// Identifies any user PATH entry that belongs to this application: the dedicated
// bin folder, an old application folder that still contains the wrapper, or a
// folder containing the Caesium executable. This lets us clean up stale entries
// left behind when the application was moved to a new location.
static bool isCaesiumPathEntry(const QString& part)
{
    const QString clean = QDir::cleanPath(QDir::toNativeSeparators(part));
    if (clean.isEmpty() || clean == QLatin1String(".")) {
        return false;
    }
    // The dedicated single-file bin folder.
    if (clean.compare(QDir::cleanPath(binDir()), Qt::CaseInsensitive) == 0) {
        return true;
    }
    // Any folder that still contains the wrapper (old app folders).
    if (QFileInfo::exists(QDir(clean).filePath(QStringLiteral("caesium-cli.cmd")))) {
        return true;
    }
    // Any folder that contains the Caesium executable.
    if (QFileInfo::exists(QDir(clean).filePath(QStringLiteral("Caesium Image Compressor.exe")))) {
        return true;
    }
    return false;
}

// The command is considered configured when the dedicated bin folder is on PATH,
// the wrapper file actually exists there, AND the recorded executable location
// still exists on disk. The last check prevents a stale entry (e.g. after the
// application was moved) from being reported as working.
bool isConfigured()
{
    const QString dir = QDir::cleanPath(binDir());
    const QStringList parts = userPathValue().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (QDir::cleanPath(QDir::toNativeSeparators(part)).compare(dir, Qt::CaseInsensitive) == 0) {
            return QFile::exists(cliWrapperPath()) && QFile::exists(readAppPath());
        }
    }
    return false;
}

bool addToPath()
{
    if (!isSupported()) {
        return false;
    }
    if (!ensureWrapper()) {
        return false;
    }
    const QString dir = QDir::cleanPath(binDir());
    const QStringList parts = userPathValue().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QStringList kept;
    bool changed = false;
    bool dirPresent = false;
    for (const QString& part : parts) {
        const QString clean = QDir::cleanPath(QDir::toNativeSeparators(part));
        if (isCaesiumPathEntry(part)) {
            if (clean.compare(dir, Qt::CaseInsensitive) == 0) {
                if (!dirPresent) {
                    kept << part;
                    dirPresent = true;
                } else {
                    changed = true; // duplicate bin folder entry dropped
                }
            } else {
                changed = true; // stale entry from an old app location dropped
            }
        } else {
            kept << part;
        }
    }
    if (!dirPresent) {
        kept << cliFolder();
        changed = true;
    }
    if (!changed) {
        return true;
    }
    return setUserPathValue(kept.join(QLatin1Char(';')));
}

bool removeFromPath()
{
    if (!isSupported()) {
        return false;
    }
    const QStringList parts = userPathValue().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QStringList kept;
    bool removed = false;
    for (const QString& part : parts) {
        if (isCaesiumPathEntry(part)) {
            removed = true;
        } else {
            kept << part;
        }
    }
    if (!removed) {
        return true;
    }
    return setUserPathValue(kept.join(QLatin1Char(';')));
}

bool launchAppDetached()
{
#ifdef Q_OS_WIN
    QProcess launcher;
    launcher.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        // Do not let the GUI inherit the terminal's std handles, so
        // "caesium-cli ... | other-command" pipelines do not hang waiting for
        // EOF while the GUI stays open in the background.
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
        args->startupInfo->hStdInput = nullptr;
        args->startupInfo->hStdOutput = nullptr;
        args->startupInfo->hStdError = nullptr;
    });
    return launcher.startDetached(QCoreApplication::applicationFilePath(), QStringList());
#else
    return QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
#endif
}

} // namespace CliPathConfig
