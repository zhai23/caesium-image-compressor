#include "CliPathConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

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

QString cliFolder()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
}

QString cliWrapperPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("caesium-cli.cmd"));
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

// Writes a minimal "caesium-cli.cmd" next to the executable if it is missing,
// so "Add to PATH" always leaves a working "caesium-cli" command behind.
static bool ensureWrapper()
{
    const QString path = cliWrapperPath();
    if (QFile::exists(path)) {
        return true;
    }
    const QString content = QStringLiteral(
        "@echo off\r\n"
        "rem Caesium Image Compressor - command line control\r\n"
        "setlocal\r\n"
        "if not exist \"%~dp0Caesium Image Compressor.exe\" (\r\n"
        "    echo Error: could not find \"%~dp0Caesium Image Compressor.exe\" 1>&2\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "set \"OUTFILE=%TEMP%\\caesium-cli-%RANDOM%-%RANDOM%.out\"\r\n"
        "\"%~dp0Caesium Image Compressor.exe\" --cli %* > \"%OUTFILE%\" 2>&1\r\n"
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

bool isConfigured()
{
    const QString folder = QDir::cleanPath(cliFolder());
    const QStringList parts = userPathValue().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (QDir::cleanPath(QDir::toNativeSeparators(part)).compare(folder, Qt::CaseInsensitive) == 0) {
            return true;
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
    if (isConfigured()) {
        return true;
    }
    QString value = userPathValue();
    if (!value.isEmpty() && !value.endsWith(QLatin1Char(';'))) {
        value += QLatin1Char(';');
    }
    value += cliFolder();
    return setUserPathValue(value);
}

bool removeFromPath()
{
    if (!isSupported()) {
        return false;
    }
    const QString folder = QDir::cleanPath(cliFolder());
    const QStringList parts = userPathValue().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QStringList kept;
    bool removed = false;
    for (const QString& part : parts) {
        if (QDir::cleanPath(QDir::toNativeSeparators(part)).compare(folder, Qt::CaseInsensitive) == 0) {
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
