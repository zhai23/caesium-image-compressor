#include "CliRunner.h"

#include "CliClient.h"
#include "utils/CliPathConfig.h"
#include "utils/Utils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QThread>

#include <cstdio>
#include <iostream>

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_OPERATION_ERROR = 1;
constexpr int EXIT_NOT_RUNNING = 2;
constexpr int EXIT_BAD_USAGE = 3;

// How long to wait for the application to start before giving up.
constexpr int kAutoLaunchTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool flagEnabled(QStringList& args, const QString& flag)
{
    return args.removeAll(flag) > 0;
}

QString jsonValueToString(const QJsonValue& v)
{
    if (v.isString()) {
        return v.toString();
    }
    if (v.isDouble()) {
        return QString::number(v.toDouble());
    }
    if (v.isBool()) {
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (v.isArray()) {
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    }
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    return QStringLiteral("null");
}

// Machine-readable output. By default it is written in the system's local
// 8-bit encoding (e.g. GBK on Chinese Windows), exactly like the human-readable
// output, so it displays correctly in a Chinese console without switching code
// pages. Scripts / AI that need strict UTF-8 can set CAESIUM_CLI_UTF8=1.
void writeJsonOutput(const QJsonObject& obj)
{
    // Indented output so it is readable in a console; still valid JSON for
    // machines / AI.
    const QByteArray utf8 = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    QByteArray bytes = utf8;
    if (!qEnvironmentVariableIsSet("CAESIUM_CLI_UTF8")) {
        bytes = QString::fromUtf8(utf8).toLocal8Bit();
    }
    fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    fputc('\n', stdout);
}

// For --json: print the structured result/error for machines to consume.
void printJsonIfNeeded(const CliResponse& resp, bool json)
{
    if (!json) {
        return;
    }
    QJsonObject out;
    out[QStringLiteral("ok")] = resp.ok;
    if (resp.ok) {
        out[QStringLiteral("result")] = resp.result;
    } else {
        out[QStringLiteral("code")] = resp.code;
        out[QStringLiteral("message")] = resp.message;
    }
    writeJsonOutput(out);
}

// Shared error handling + exit-code mapping.
int finishResponse(const CliResponse& resp, bool json)
{
    printJsonIfNeeded(resp, json);
    if (resp.ok) {
        return EXIT_OK;
    }
    if (resp.code == QStringLiteral("NOT_RUNNING")) {
        fprintf(stderr, "Caesium Image Compressor is not running.\n");
        fprintf(stderr, "Start the application first, then run this command again.\n");
        return EXIT_NOT_RUNNING;
    }
    fprintf(stderr, "Error: %s\n", qPrintable(resp.message.isEmpty() ? resp.code : resp.message));
    return EXIT_OPERATION_ERROR;
}

// Sends a command to the running application. When the app is not running and
// autoLaunch is enabled, starts the application, waits until its CLI pipe is
// ready, then re-sends the command.
CliResponse sendWithAutoLaunch(const QString& method, const QJsonObject& params, bool autoLaunch)
{
    CliResponse response = CliClient::sendRequest(method, params);
    if (!autoLaunch || response.code != QStringLiteral("NOT_RUNNING")) {
        return response;
    }

    fprintf(stderr, "Caesium is not running. Starting it...\n");
    if (!CliPathConfig::launchAppDetached()) {
        fprintf(stderr, "Could not start Caesium.\n");
        return response;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kAutoLaunchTimeoutMs) {
        if (CliClient::isRunning()) {
            break;
        }
        QThread::msleep(200);
    }
    if (!CliClient::isRunning()) {
        fprintf(stderr, "Caesium did not become ready in time.\n");
        return response;
    }

    return CliClient::sendRequest(method, params);
}

QString friendlyStatus(const QString& status)
{
    if (status == QLatin1String("compressing")) {
        return QStringLiteral("compressing");
    }
    if (status == QLatin1String("compressed")) {
        return QStringLiteral("compressed");
    }
    if (status == QLatin1String("error")) {
        return QStringLiteral("error");
    }
    if (status == QLatin1String("warning")) {
        return QStringLiteral("warning");
    }
    return QStringLiteral("waiting");
}

// ---------------------------------------------------------------------------
// Human-readable printers
// ---------------------------------------------------------------------------

// Fits a name into "maxWidth" display columns of a Chinese (GBK) console.
// CJK characters take 2 columns (2 bytes in GBK), ASCII takes 1. When the name
// is too long it is cut at a safe boundary and "..." is appended, so the
// result is always at most maxWidth bytes/columns and table columns stay
// aligned.
QByteArray fitName(const QString& name, int maxWidth)
{
    // Measure the full display width first (CJK chars are 2 bytes / columns in
    // a GBK console, ASCII is 1).
    int fullWidth = 0;
    for (const QChar& ch : name) {
        fullWidth += QString(ch).toLocal8Bit().size();
    }
    if (fullWidth <= maxWidth) {
        return name.toLocal8Bit(); // fits entirely, no ellipsis
    }
    // Too long: reserve space for "..." and cut at a safe character boundary.
    QByteArray out;
    int width = 0;
    for (const QChar& ch : name) {
        const QByteArray charBytes = QString(ch).toLocal8Bit();
        const int charWidth = charBytes.size();
        if (width + charWidth > maxWidth - 3) {
            break;
        }
        out += charBytes;
        width += charWidth;
    }
    out += "...";
    return out;
}

void printListHuman(const CliResponse& resp)
{
    const QJsonArray items = resp.result.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) {
        printf("No images in the list.\n");
        return;
    }
    printf("%-30s  %-6s  %-12s  %10s  %8s\n", "Name", "Format", "Status", "Original", "Saved");
    printf("%s\n", QString(74, QLatin1Char('-')).toUtf8().constData());
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        QString format = item.value(QStringLiteral("format")).toString();
        if (format.isEmpty()) {
            // Fall back to the file extension when the app is an older build
            // that does not report the format field yet.
            format = QFileInfo(name).suffix();
        }
        format = format.toUpper();
        const QString status = friendlyStatus(item.value(QStringLiteral("status")).toString());
        const double original = item.value(QStringLiteral("original_size")).toDouble();
        const QString saved = item.value(QStringLiteral("saved")).toString();

        const QByteArray displayName = fitName(name, 30);

        printf("%-30s  %-6s  %-12s  %10s  %8s\n",
            displayName.constData(),
            qPrintable(format),
            qPrintable(status),
            qPrintable(toHumanSize(original)),
            qPrintable(saved));
    }
}

void printStatusHuman(const CliResponse& resp)
{
    const QJsonObject r = resp.result;
    const bool compressing = r.value(QStringLiteral("compressing")).toBool();
    const int images = r.value(QStringLiteral("images")).toInt();
    if (compressing) {
        const QJsonObject progress = r.value(QStringLiteral("progress")).toObject();
        printf("Compressing... %d/%d\n",
            progress.value(QStringLiteral("current")).toInt(),
            progress.value(QStringLiteral("total")).toInt());
    } else {
        printf("Idle. %d image(s) in the list.\n", images);
    }
    const QJsonObject counts = r.value(QStringLiteral("counts")).toObject();
    printf("  compressed: %d   errors: %d   waiting: %d\n",
        counts.value(QStringLiteral("compressed")).toInt(),
        counts.value(QStringLiteral("error")).toInt(),
        counts.value(QStringLiteral("uncompressed")).toInt());
}

// Blocks until the running compression finishes, then prints the summary.
int waitAndPrintSummary(bool json)
{
    while (true) {
        const CliResponse status = CliClient::sendRequest(QStringLiteral("status"), {});
        if (!status.ok) {
            if (!json) {
                fprintf(stderr, "\n");
            }
            return finishResponse(status, json);
        }
        if (!status.result.value(QStringLiteral("compressing")).toBool()) {
            if (json) {
                printJsonIfNeeded(status, true);
                return EXIT_OK;
            }
            fprintf(stderr, "\n");
            const QJsonArray items = status.result.value(QStringLiteral("items")).toArray();
            qint64 original = 0;
            qint64 compressed = 0;
            int errors = 0;
            for (const QJsonValue& value : items) {
                const QJsonObject item = value.toObject();
                original += static_cast<qint64>(item.value(QStringLiteral("original_size")).toDouble());
                if (item.value(QStringLiteral("status")).toString() == QLatin1String("compressed")) {
                    compressed += static_cast<qint64>(item.value(QStringLiteral("compressed_size")).toDouble());
                }
                if (item.value(QStringLiteral("status")).toString() == QLatin1String("error")) {
                    errors++;
                }
            }
            const qint64 saved = original - compressed;
            const double pct = original > 0 ? (saved * 100.0 / static_cast<double>(original)) : 0.0;
            printf("Compression finished.\n");
            printf("  Images:      %d\n", items.size());
            printf("  Original:    %s\n", qPrintable(toHumanSize(static_cast<double>(original))));
            printf("  Compressed:  %s\n", qPrintable(toHumanSize(static_cast<double>(compressed))));
            printf("  Saved:       %s (%.1f%%)\n", qPrintable(toHumanSize(static_cast<double>(saved))), pct);
            if (errors > 0) {
                printf("  Errors:      %d\n", errors);
            }
            return EXIT_OK;
        }
        const QJsonObject progress = status.result.value(QStringLiteral("progress")).toObject();
        if (!json) {
            fprintf(stderr, "\rCompressing... %d/%d",
                progress.value(QStringLiteral("current")).toInt(),
                progress.value(QStringLiteral("total")).toInt());
            fflush(stderr);
        }
        QThread::msleep(250);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int runCompress(QStringList args, bool json, bool autoLaunch)
{
    // Map CLI flags to the option keys understood by the application.
    static const QHash<QString, QString> valueFlags = {
        { QStringLiteral("--format"), QStringLiteral("format") },
        { QStringLiteral("-f"), QStringLiteral("format") },
        { QStringLiteral("--quality"), QStringLiteral("quality") },
        { QStringLiteral("-q"), QStringLiteral("quality") },
        { QStringLiteral("--jpeg-quality"), QStringLiteral("jpeg_quality") },
        { QStringLiteral("--png-quality"), QStringLiteral("png_quality") },
        { QStringLiteral("--webp-quality"), QStringLiteral("webp_quality") },
        { QStringLiteral("--output"), QStringLiteral("output_folder") },
        { QStringLiteral("-o"), QStringLiteral("output_folder") },
        { QStringLiteral("--suffix"), QStringLiteral("output_suffix") },
        { QStringLiteral("--fit"), QStringLiteral("fit_to") },
        { QStringLiteral("--mode"), QStringLiteral("mode") },
        { QStringLiteral("--width"), QStringLiteral("width") },
        { QStringLiteral("--height"), QStringLiteral("height") },
        { QStringLiteral("--edge"), QStringLiteral("edge") },
        { QStringLiteral("--max-output-size"), QStringLiteral("max_output_size") },
        { QStringLiteral("--max-output-size-unit"), QStringLiteral("max_output_size_unit") },
    };
    static const QSet<QString> boolFlags = {
        QStringLiteral("--lossless"), QStringLiteral("--keep-metadata"), QStringLiteral("--keep-structure"),
        QStringLiteral("--keep-dates"), QStringLiteral("--same-folder-as-input"), QStringLiteral("--skip-if-bigger"),
        QStringLiteral("--cache-originals"), QStringLiteral("--delete-original-format-file"),
        QStringLiteral("--keep-aspect-ratio"), QStringLiteral("--do-not-enlarge"), QStringLiteral("--jpeg-progressive"),
    };

    QStringList paths;
    QJsonObject options;
    bool onlyFailed = false;
    bool wait = false;
    bool dryRun = false;

    for (int i = 0; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QLatin1String("--json")) {
            continue;
        }
        if (a == QLatin1String("--wait")) {
            wait = true;
            continue;
        }
        if (a == QLatin1String("--only-failed")) {
            onlyFailed = true;
            continue;
        }
        if (a == QLatin1String("--dry-run")) {
            dryRun = true;
            continue;
        }
        if (boolFlags.contains(a)) {
            const QString key = a.mid(2).replace(QLatin1Char('-'), QLatin1Char('_'));
            options[key] = true;
            continue;
        }
        if (valueFlags.contains(a)) {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "Error: missing value for %s\n", qPrintable(a));
                return EXIT_BAD_USAGE;
            }
            const QString key = valueFlags.value(a);
            const QString value = args.at(++i);
            if (key == QLatin1String("quality")) {
                bool okNum = false;
                const int q = value.toInt(&okNum);
                if (!okNum) {
                    fprintf(stderr, "Error: invalid quality value: %s\n", qPrintable(value));
                    return EXIT_BAD_USAGE;
                }
                options[QStringLiteral("jpeg_quality")] = q;
                options[QStringLiteral("png_quality")] = q;
                options[QStringLiteral("webp_quality")] = q;
            } else {
                options[key] = value;
            }
            continue;
        }
        if (a.startsWith(QLatin1Char('-'))) {
            fprintf(stderr, "Error: unknown option: %s\n", qPrintable(a));
            return EXIT_BAD_USAGE;
        }
        paths << a;
    }

    if (dryRun) {
        QJsonObject out;
        out[QStringLiteral("dry_run")] = true;
        out[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
        out[QStringLiteral("options")] = options;
        if (json) {
            printJsonIfNeeded({ true, {}, {}, out }, true);
        } else {
            printf("Dry run: would process %d path(s).\n", paths.size());
            printf("Options:\n");
            for (auto it = options.begin(); it != options.end(); ++it) {
                printf("  %s = %s\n", qPrintable(it.key()), qPrintable(jsonValueToString(it.value())));
            }
        }
        return EXIT_OK;
    }

    // Import first so the list is populated, then apply options, then compress.
    if (!paths.isEmpty()) {
        const CliResponse importResp = sendWithAutoLaunch(QStringLiteral("import"),
            { { QStringLiteral("paths"), QJsonArray::fromStringList(paths) } }, autoLaunch);
        if (!importResp.ok) {
            if (!json) {
                fprintf(stderr, "Error importing paths: %s\n", qPrintable(importResp.message));
            }
            return finishResponse(importResp, json);
        }
    }
    if (!options.isEmpty()) {
        const CliResponse setResp = sendWithAutoLaunch(QStringLiteral("options.set"),
            { { QStringLiteral("options"), options } }, autoLaunch);
        if (!setResp.ok) {
            if (!json) {
                fprintf(stderr, "Error setting options: %s\n", qPrintable(setResp.message));
            }
            return finishResponse(setResp, json);
        }
    }

    QJsonObject params;
    if (onlyFailed) {
        params[QStringLiteral("only_failed")] = true;
    }
    const CliResponse compressResp = sendWithAutoLaunch(QStringLiteral("compress"), params, autoLaunch);
    if (!compressResp.ok) {
        if (!json) {
            fprintf(stderr, "Error: %s\n", qPrintable(compressResp.message));
        }
        return finishResponse(compressResp, json);
    }

    if (wait) {
        return waitAndPrintSummary(json);
    }

    printJsonIfNeeded(compressResp, json);
    if (!json) {
        printf("Compression started. Use 'caesium status' to check progress.\n");
    }
    return EXIT_OK;
}

int runStatusWait(bool json, bool autoLaunch)
{
    // First query may auto-launch the app; subsequent polls assume it is up.
    CliResponse status = sendWithAutoLaunch(QStringLiteral("status"), {}, autoLaunch);
    if (!status.ok) {
        return finishResponse(status, json);
    }
    while (true) {
        const bool compressing = status.result.value(QStringLiteral("compressing")).toBool();
        if (!compressing) {
            if (json) {
                printJsonIfNeeded(status, true);
            } else {
                printStatusHuman(status);
            }
            return EXIT_OK;
        }
        const QJsonObject progress = status.result.value(QStringLiteral("progress")).toObject();
        if (!json) {
            fprintf(stderr, "\rCompressing... %d/%d",
                progress.value(QStringLiteral("current")).toInt(),
                progress.value(QStringLiteral("total")).toInt());
            fflush(stderr);
        }
        QThread::msleep(250);
        status = CliClient::sendRequest(QStringLiteral("status"), {});
        if (!status.ok) {
            return finishResponse(status, json);
        }
    }
}

// Manages the user PATH entry for the application folder. Does not need the
// application to be running: it edits the user environment directly.
int runPath(QStringList args, bool json)
{
    if (args.isEmpty()) {
        fprintf(stderr, "Usage: caesium path status | add | remove\n");
        return EXIT_BAD_USAGE;
    }
    const QString sub = args.takeFirst();

    if (sub == QLatin1String("status")) {
        if (!CliPathConfig::isSupported()) {
            if (json) {
                printf("{\"ok\":true,\"result\":{\"supported\":false}}\n");
            } else {
                printf("PATH management is only available on Windows.\n");
            }
            return EXIT_OK;
        }
        const bool configured = CliPathConfig::isConfigured();
        if (json) {
            QJsonObject result;
            result[QStringLiteral("supported")] = true;
            result[QStringLiteral("configured")] = configured;
            result[QStringLiteral("folder")] = CliPathConfig::cliFolder();
            QJsonObject out;
            out[QStringLiteral("ok")] = true;
            out[QStringLiteral("result")] = result;
            writeJsonOutput(out);
        } else {
            printf("%s\n", qPrintable(CliPathConfig::cliFolder()));
            if (configured) {
                printf("caesium-cli is on your PATH.\n");
            } else {
                printf("caesium-cli is NOT on your PATH.\n");
            }
        }
        return EXIT_OK;
    }

    if (sub == QLatin1String("add") || sub == QLatin1String("remove")) {
        const bool ok = sub == QLatin1String("add")
            ? CliPathConfig::addToPath()
            : CliPathConfig::removeFromPath();
        if (json) {
            QJsonObject result;
            result[QStringLiteral("ok")] = ok;
            result[QStringLiteral("folder")] = CliPathConfig::cliFolder();
            QJsonObject out;
            out[QStringLiteral("ok")] = true;
            out[QStringLiteral("result")] = result;
            writeJsonOutput(out);
        } else if (ok) {
            printf("Folder %s your PATH.\n", sub == QLatin1String("add") ? "added to" : "removed from");
            printf("Restart your terminal (or open a new one) for the change to take effect.\n");
        } else {
            fprintf(stderr, "Error: could not modify PATH.\n");
        }
        return ok ? EXIT_OK : EXIT_OPERATION_ERROR;
    }

    fprintf(stderr, "Usage: caesium path status | add | remove\n");
    return EXIT_BAD_USAGE;
}

int runSettings(QStringList args, bool json, bool autoLaunch)
{    if (args.isEmpty()) {
        fprintf(stderr, "Usage: caesium settings get [key] | caesium settings set <key> <value>\n");
        return EXIT_BAD_USAGE;
    }
    const QString sub = args.takeFirst();
    if (sub == QLatin1String("get")) {
        QJsonObject params;
        if (!args.isEmpty()) {
            params[QStringLiteral("key")] = args.first();
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("options.get"), params, autoLaunch);
        if (!json && resp.ok) {
            if (args.isEmpty()) {
                const QJsonObject options = resp.result.value(QStringLiteral("options")).toObject();
                for (auto it = options.begin(); it != options.end(); ++it) {
                    printf("%s = %s\n", qPrintable(it.key()), qPrintable(jsonValueToString(it.value())));
                }
            } else {
                printf("%s\n", qPrintable(jsonValueToString(resp.result.value(QStringLiteral("value")))));
            }
        }
        return finishResponse(resp, json);
    }
    if (sub == QLatin1String("set")) {
        if (args.size() < 2) {
            fprintf(stderr, "Usage: caesium settings set <key> <value>\n");
            return EXIT_BAD_USAGE;
        }
        QJsonObject options;
        options[args.at(0)] = args.at(1);
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("options.set"),
            { { QStringLiteral("options"), options } }, autoLaunch);
        if (!json && resp.ok) {
            printf("Option '%s' set.\n", qPrintable(args.at(0)));
        }
        return finishResponse(resp, json);
    }
    fprintf(stderr, "Error: unknown settings subcommand: %s\n", qPrintable(sub));
    return EXIT_BAD_USAGE;
}

int runShell(bool json, bool autoLaunch)
{
    const CliResponse ping = sendWithAutoLaunch(QStringLiteral("ping"), {}, autoLaunch);
    if (!ping.ok) {
        fprintf(stderr, "Caesium Image Compressor is not running. Start it first.\n");
        return EXIT_NOT_RUNNING;
    }
    printf("Caesium CLI shell. Type 'help' for commands, 'exit' to quit.\n");
    std::string line;
    while (true) {
        printf("caesium> ");
        fflush(stdout);
        if (!std::getline(std::cin, line)) {
            break;
        }
        const QStringList args = QProcess::splitCommand(QString::fromStdString(line));
        if (args.isEmpty()) {
            continue;
        }
        if (args.first() == QLatin1String("exit") || args.first() == QLatin1String("quit")) {
            break;
        }
        (void)CliRunner::run(args);
    }
    return EXIT_OK;
}

void printHelp()
{
    printf("Caesium Image Compressor - command line control for the running app.\n\n");
    printf("Usage:\n");
    printf("  caesium --cli <command> [options]\n\n");
    printf("Commands:\n");
    printf("  add <paths...>              Add files/folders to the list\n");
    printf("  remove <paths...>           Remove matching items from the list\n");
    printf("  clear                       Clear the whole list\n");
    printf("  list                        Show the current list\n");
    printf("  status [--wait]             Show progress and per-image status\n");
    printf("  compress [paths...] [opts]  Add + configure + start compression\n");
    printf("  cancel                      Cancel the running compression\n");
    printf("  settings get [key]          Read compression options\n");
    printf("  settings set <key> <value>  Write a compression option\n");
    printf("  path status|add|remove      Manage the CLI folder on your PATH\n");
    printf("  show                        Bring the main window to the front\n");
    printf("  quit                        Close the application\n");
    printf("  shell                       Interactive session connected to the app\n");
    printf("  help                        Show this help\n\n");
    printf("Compress options:\n");
    printf("  -q, --quality <0-100>       JPEG/PNG/WebP quality\n");
    printf("  --format <jpg|png|webp|tiff|same>\n");
    printf("  -o, --output <dir>          Output folder\n");
    printf("  --suffix <text>             Output file suffix\n");
    printf("  --fit <none|dimensions|percentage|short_edge|long_edge|fixed_width|fixed_height>\n");
    printf("  --width <px>, --height <px>, --edge <px>\n");
    printf("  --mode <quality|size>\n");
    printf("  --lossless, --keep-metadata, --keep-dates, --same-folder-as-input\n");
    printf("  --skip-if-bigger, --cache-originals, --do-not-enlarge\n");
    printf("  --only-failed               Recompress only the failed images\n");
    printf("  --wait                      Wait for compression and print the summary\n");
    printf("  --dry-run                   Preview what would happen without doing it\n\n");
    printf("Global options:\n");
    printf("  --json                      Machine-readable JSON output\n");
    printf("  --no-launch                 Do not auto-start Caesium if it is not running\n");
    printf("  --raw <json>                Send a raw command: {\"method\":\"...\",\"params\":{...}}\n\n");
    printf("By default the application is started automatically (if it is not\n");
    printf("already running) before the command is executed.\n\n");
    printf("Examples:\n");
    printf("  caesium --cli compress ./photos -q 80 -o ./out --wait\n");
    printf("  caesium --cli status --json\n");
    printf("  caesium --cli settings set jpeg_quality 85\n");
}

} // namespace

int CliRunner::run(const QStringList& argv)
{
    QStringList args = argv;
    const bool json = flagEnabled(args, QStringLiteral("--json"));
    const bool autoLaunch = !flagEnabled(args, QStringLiteral("--no-launch"));

    if (args.isEmpty() || args.contains(QStringLiteral("help"))
        || args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        printHelp();
        return EXIT_OK;
    }
    if (args.contains(QStringLiteral("--version"))) {
        printf("Caesium Image Compressor %s\n", qPrintable(QCoreApplication::applicationVersion()));
        return EXIT_OK;
    }

    if (args.first() == QLatin1String("--raw")) {
        args.removeFirst();
        const QString raw = args.join(QLatin1Char(' '));
        if (raw.trimmed().isEmpty()) {
            fprintf(stderr, "Error: --raw requires a JSON object.\n");
            return EXIT_BAD_USAGE;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            fprintf(stderr, "Error: invalid JSON for --raw.\n");
            return EXIT_BAD_USAGE;
        }
        const QJsonObject req = doc.object();
        const QString method = req.value(QStringLiteral("method")).toString();
        const QJsonObject params = req.value(QStringLiteral("params")).toObject();
        if (method.isEmpty()) {
            fprintf(stderr, "Error: --raw requires a 'method' field.\n");
            return EXIT_BAD_USAGE;
        }
        const CliResponse resp = sendWithAutoLaunch(method, params, autoLaunch);
        if (resp.ok && !json) {
            // Raw commands have no friendly printer; show the JSON result.
            writeJsonOutput(resp.result);
        }
        return finishResponse(resp, json);
    }

    const QString command = args.takeFirst();

    if (command == QLatin1String("add")) {
        if (args.isEmpty()) {
            fprintf(stderr, "Usage: caesium add <paths...>\n");
            return EXIT_BAD_USAGE;
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("import"),
            { { QStringLiteral("paths"), QJsonArray::fromStringList(args) } }, autoLaunch);
        if (!json && resp.ok) {
            printf("Imported %d image(s).\n", resp.result.value(QStringLiteral("added")).toInt());
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("remove")) {
        if (args.isEmpty()) {
            fprintf(stderr, "Usage: caesium remove <paths...>\n");
            return EXIT_BAD_USAGE;
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("remove"),
            { { QStringLiteral("paths"), QJsonArray::fromStringList(args) } }, autoLaunch);
        if (!json && resp.ok) {
            printf("Removed %d image(s).\n", resp.result.value(QStringLiteral("removed")).toInt());
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("clear")) {
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("clear"), {}, autoLaunch);
        if (!json && resp.ok) {
            printf("Cleared %d image(s).\n", resp.result.value(QStringLiteral("cleared")).toInt());
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("list")) {
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("list"), {}, autoLaunch);
        if (!json && resp.ok) {
            printListHuman(resp);
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("status")) {
        const bool wait = flagEnabled(args, QStringLiteral("--wait"));
        if (wait) {
            return runStatusWait(json, autoLaunch);
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("status"), {}, autoLaunch);
        if (!json && resp.ok) {
            printStatusHuman(resp);
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("compress")) {
        return runCompress(args, json, autoLaunch);
    }

    if (command == QLatin1String("cancel")) {
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("cancel"), {}, autoLaunch);
        if (!json && resp.ok) {
            printf("Compression canceled.\n");
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("settings")) {
        return runSettings(args, json, autoLaunch);
    }

    if (command == QLatin1String("path")) {
        return runPath(args, json);
    }

    if (command == QLatin1String("show")) {
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("show"), {}, autoLaunch);
        if (!json && resp.ok) {
            printf("OK\n");
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("quit")) {
        // Quitting an app that is not running is a no-op; do not auto-launch.
        const CliResponse resp = CliClient::sendRequest(QStringLiteral("quit"), {});
        if (!json && resp.ok) {
            printf("OK\n");
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("shell")) {
        return runShell(json, autoLaunch);
    }

    fprintf(stderr, "Unknown command: %s\n", qPrintable(command));
    fprintf(stderr, "Try 'caesium --cli help' for a list of commands.\n");
    return EXIT_BAD_USAGE;
}
