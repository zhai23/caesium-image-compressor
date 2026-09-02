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

#ifdef _WIN32
#include <conio.h>
#include <cwchar>
#include <io.h>

// windows.h cannot be included here: the project's own enum CompressionMode
// defines a SIZE enumerator which clashes with the Windows SIZE typedef.
// Declare just the one function we need.
extern "C" {
__declspec(dllimport) wchar_t* __stdcall GetCommandLineW(void);
}
#endif

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

// ---------------------------------------------------------------------------
// Output helpers. All human-readable text is written in the console's local
// 8-bit encoding (GBK on Chinese Windows) so Chinese displays correctly without
// any codepage switching.
// ---------------------------------------------------------------------------

void printOut(const QString& text)
{
    const QByteArray bytes = text.toLocal8Bit();
    fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
}

void printOutLn(const QString& text)
{
    printOut(text);
    fputc('\n', stdout);
}

void printErrLn(const QString& text)
{
    const QByteArray bytes = text.toLocal8Bit();
    fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    fputc('\n', stderr);
}

// Progress line on stderr without a trailing newline ("\r" based).
void printProgress(const QString& text)
{
    const QByteArray bytes = text.toLocal8Bit();
    fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    fflush(stderr);
}

// Splits CLI arguments into numeric image IDs and file paths. Anything that
// parses as a positive integer is treated as an ID; everything else as a path.
void splitIdsAndPaths(const QStringList& args, QList<int>& ids, QStringList& paths)
{
    for (const QString& a : args) {
        bool ok = false;
        const int id = a.toInt(&ok);
        if (ok && id > 0) {
            ids << id;
        } else {
            paths << a;
        }
    }
}

// Rebuilds the "--raw" payload from the process's raw command line. cmd's %*
// expansion inside the .cmd wrapper strips the embedded quotes from a JSON
// argument before the executable sees it, so QApplication::arguments() gets a
// mangled JSON object (e.g. {method:...} instead of {"method":...}). On
// Windows the raw command line still contains the original quotes, so we can
// recover the exact payload from there.
QString recoverRawPayload()
{
#ifdef _WIN32
    const wchar_t* cmdLine = ::GetCommandLineW();
    if (cmdLine == nullptr) {
        return QString();
    }
    const QString full = QString::fromWCharArray(cmdLine);
    // The command line looks like:
    //   "C:\...\Caesium Image Compressor.exe" --cli [global flags] --raw {"method":...}
    // Locate the "--raw" token that belongs to this invocation (after "--cli").
    const int cliPos = full.indexOf(QLatin1String("--cli"));
    const int rawPos = full.indexOf(QLatin1String("--raw"), cliPos < 0 ? 0 : cliPos);
    if (rawPos < 0) {
        return QString();
    }
    QString payload = full.mid(rawPos + qstrlen("--raw")).trimmed();
    // Drop any trailing global options that may follow the JSON (e.g. --json).
    while (payload.endsWith(QLatin1String(" --json"))
        || payload.endsWith(QLatin1String(" --no-launch"))
        || payload == QLatin1String("--json")
        || payload == QLatin1String("--no-launch")) {
        const int sp = payload.lastIndexOf(QLatin1Char(' '));
        payload = (sp < 0) ? QString() : payload.left(sp).trimmed();
    }
    return payload;
#else
    return QString();
#endif
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
        printErrLn(QStringLiteral("Caesium Image Compressor 未在运行。"));
        printErrLn(QStringLiteral("请先启动软件，然后再执行该命令。"));
        return EXIT_NOT_RUNNING;
    }
    printErrLn(QStringLiteral("错误：%1").arg(resp.message.isEmpty() ? resp.code : resp.message));
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

    printErrLn(QStringLiteral("Caesium 未在运行，正在启动..."));
    if (!CliPathConfig::launchAppDetached()) {
        printErrLn(QStringLiteral("无法启动 Caesium。"));
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
        printErrLn(QStringLiteral("Caesium 未能及时就绪。"));
        return response;
    }

    return CliClient::sendRequest(method, params);
}

// Chinese labels for the human-readable table / status output.
QString statusHuman(const QString& status)
{
    if (status == QLatin1String("compressing")) {
        return QStringLiteral("压缩中");
    }
    if (status == QLatin1String("compressed")) {
        return QStringLiteral("已压缩");
    }
    if (status == QLatin1String("error")) {
        return QStringLiteral("错误");
    }
    if (status == QLatin1String("warning")) {
        return QStringLiteral("警告");
    }
    return QStringLiteral("等待");
}

// ---------------------------------------------------------------------------
// Chinese annotations for option listings (so the CLI reads like the GUI's
// option panel: a Chinese label in front of every English parameter name).
// ---------------------------------------------------------------------------

// Returns the Chinese description for a CLI option key, or an empty string.
QString cliOptionZh(const QString& key)
{
    static const QHash<QString, QString> zh = {
        { QStringLiteral("format"), QStringLiteral("输出格式") },
        { QStringLiteral("mode"), QStringLiteral("压缩模式") },
        { QStringLiteral("lossless"), QStringLiteral("无损压缩") },
        { QStringLiteral("keep_metadata"), QStringLiteral("保留元数据") },
        { QStringLiteral("jpeg_quality"), QStringLiteral("JPEG 质量") },
        { QStringLiteral("jpeg_chroma_subsampling"), QStringLiteral("JPEG 色度采样") },
        { QStringLiteral("jpeg_progressive"), QStringLiteral("渐进式 JPEG") },
        { QStringLiteral("png_quality"), QStringLiteral("PNG 质量") },
        { QStringLiteral("png_optimization_level"), QStringLiteral("PNG 优化级别") },
        { QStringLiteral("webp_quality"), QStringLiteral("WebP 质量") },
        { QStringLiteral("tiff_method"), QStringLiteral("TIFF 压缩方式") },
        { QStringLiteral("tiff_deflate_level"), QStringLiteral("TIFF Deflate 级别") },
        { QStringLiteral("max_output_size"), QStringLiteral("目标输出大小") },
        { QStringLiteral("max_output_size_unit"), QStringLiteral("大小单位") },
        { QStringLiteral("fit_to"), QStringLiteral("缩放方式") },
        { QStringLiteral("width"), QStringLiteral("宽度") },
        { QStringLiteral("height"), QStringLiteral("高度") },
        { QStringLiteral("edge"), QStringLiteral("边长") },
        { QStringLiteral("keep_aspect_ratio"), QStringLiteral("保持宽高比") },
        { QStringLiteral("do_not_enlarge"), QStringLiteral("不放大") },
        { QStringLiteral("output_folder"), QStringLiteral("输出文件夹") },
        { QStringLiteral("output_suffix"), QStringLiteral("输出后缀") },
        { QStringLiteral("same_folder_as_input"), QStringLiteral("输出到输入目录") },
        { QStringLiteral("skip_if_bigger"), QStringLiteral("变大则跳过") },
        { QStringLiteral("cache_originals"), QStringLiteral("缓存原始文件") },
        { QStringLiteral("delete_original_format_file"), QStringLiteral("转换后删除原文件") },
        { QStringLiteral("keep_dates"), QStringLiteral("保留日期") },
        { QStringLiteral("keep_creation_date"), QStringLiteral("保留创建日期") },
        { QStringLiteral("keep_last_modified_date"), QStringLiteral("保留修改日期") },
        { QStringLiteral("keep_last_access_date"), QStringLiteral("保留访问日期") },
    };
    return zh.value(key);
}

// Returns the Chinese category of an option key (mirrors the GUI's
// Compression / Resize / Output tabs), or an empty string for unknown keys.
QString cliOptionCategory(const QString& key)
{
    static const QSet<QString> compression = {
        QStringLiteral("mode"), QStringLiteral("lossless"), QStringLiteral("keep_metadata"),
        QStringLiteral("jpeg_quality"), QStringLiteral("jpeg_chroma_subsampling"),
        QStringLiteral("jpeg_progressive"), QStringLiteral("png_quality"),
        QStringLiteral("png_optimization_level"), QStringLiteral("webp_quality"),
        QStringLiteral("tiff_method"), QStringLiteral("tiff_deflate_level"),
        QStringLiteral("max_output_size"), QStringLiteral("max_output_size_unit"),
    };
    static const QSet<QString> resize = {
        QStringLiteral("fit_to"), QStringLiteral("width"), QStringLiteral("height"),
        QStringLiteral("edge"), QStringLiteral("keep_aspect_ratio"), QStringLiteral("do_not_enlarge"),
    };
    static const QSet<QString> output = {
        QStringLiteral("format"), QStringLiteral("output_folder"), QStringLiteral("output_suffix"),
        QStringLiteral("same_folder_as_input"), QStringLiteral("skip_if_bigger"),
        QStringLiteral("cache_originals"), QStringLiteral("delete_original_format_file"),
        QStringLiteral("keep_dates"), QStringLiteral("keep_creation_date"),
        QStringLiteral("keep_last_modified_date"), QStringLiteral("keep_last_access_date"),
    };
    if (compression.contains(key)) {
        return QStringLiteral("压缩");
    }
    if (resize.contains(key)) {
        return QStringLiteral("缩放");
    }
    if (output.contains(key)) {
        return QStringLiteral("输出");
    }
    return QString();
}

// Prints a set of options grouped by category. Each line is
// `    中文说明：key = value`, so every English parameter name keeps a Chinese
// comment in front of it, like the labels in the GUI option panel.
void printOptionsGrouped(const QJsonObject& options)
{
    static const QStringList categories = {
        QStringLiteral("压缩"), QStringLiteral("缩放"), QStringLiteral("输出"),
    };
    QSet<QString> printed;
    for (const QString& cat : categories) {
        bool first = true;
        for (auto it = options.begin(); it != options.end(); ++it) {
            if (cliOptionCategory(it.key()) != cat) {
                continue;
            }
            if (first) {
                printOutLn(QStringLiteral("  %1：").arg(cat));
                first = false;
            }
            printed.insert(it.key());
            printOutLn(QStringLiteral("    %1：%2 = %3")
                .arg(cliOptionZh(it.key()), it.key(), jsonValueToString(it.value())));
        }
    }
    // Options without a known category are still shown so nothing is hidden.
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (printed.contains(it.key())) {
            continue;
        }
        printOutLn(QStringLiteral("    %1 = %2").arg(it.key(), jsonValueToString(it.value())));
    }
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
        printOutLn(QStringLiteral("列表中没有图片。"));
        return;
    }
    // The header cells are converted to the local encoding; the printf layout
    // string stays ASCII so %-paddings keep working.
    printf("%4s  %-30s  %-6s  %-4s  %-12s  %10s  %8s\n",
        qPrintable(QStringLiteral("ID")),
        qPrintable(QStringLiteral("名称")),
        qPrintable(QStringLiteral("格式")),
        qPrintable(QStringLiteral("已选")),
        qPrintable(QStringLiteral("状态")),
        qPrintable(QStringLiteral("原始大小")),
        qPrintable(QStringLiteral("节省")));
    printf("%s\n", QString(86, QLatin1Char('-')).toUtf8().constData());
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        const int id = item.value(QStringLiteral("id")).toInt();
        const QString name = item.value(QStringLiteral("name")).toString();
        QString format = item.value(QStringLiteral("format")).toString();
        if (format.isEmpty()) {
            // Fall back to the file extension when the app is an older build
            // that does not report the format field yet.
            format = QFileInfo(name).suffix();
        }
        format = format.toUpper();
        const bool checked = item.value(QStringLiteral("checked")).toBool(true);
        const QString selText = checked ? QStringLiteral("是") : QStringLiteral("否");
        const QString status = statusHuman(item.value(QStringLiteral("status")).toString());
        const double original = item.value(QStringLiteral("original_size")).toDouble();
        const QString saved = item.value(QStringLiteral("saved")).toString();

        const QByteArray displayName = fitName(name, 30);

        printf("%4s  %-30s  %-6s  %-4s  %-12s  %10s  %8s\n",
            qPrintable(QString::number(id)),
            displayName.constData(),
            qPrintable(format),
            qPrintable(selText),
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
        printOutLn(QStringLiteral("正在压缩... %1/%2")
            .arg(progress.value(QStringLiteral("current")).toInt())
            .arg(progress.value(QStringLiteral("total")).toInt()));
    } else {
        printOutLn(QStringLiteral("空闲。列表中共 %1 张图片。").arg(images));
    }
    const double origTotal = r.value(QStringLiteral("total_original_size")).toDouble();
    const double compTotal = r.value(QStringLiteral("total_compressed_size")).toDouble();
    printOutLn(QStringLiteral("  原始大小：%1   压缩后：%2")
        .arg(toHumanSize(origTotal))
        .arg(toHumanSize(compTotal)));
    const QJsonObject counts = r.value(QStringLiteral("counts")).toObject();
    printOutLn(QStringLiteral("  已压缩：%1   错误：%2   等待：%3")
        .arg(counts.value(QStringLiteral("compressed")).toInt())
        .arg(counts.value(QStringLiteral("error")).toInt())
        .arg(counts.value(QStringLiteral("uncompressed")).toInt()));
    if (r.value(QStringLiteral("multi_selection")).toBool()) {
        printOutLn(QStringLiteral("  已选：%1 / %2")
            .arg(r.value(QStringLiteral("selected")).toInt())
            .arg(images));
    }
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
            int count = 0;
            int errors = 0;
            for (const QJsonValue& value : items) {
                const QJsonObject item = value.toObject();
                const QString st = item.value(QStringLiteral("status")).toString();
                // Only items that actually went through this compression run
                // count toward the summary. When compressing a subset (by ID or
                // path), the untouched images keep their "uncompressed" status
                // and are excluded.
                if (st != QLatin1String("compressed") && st != QLatin1String("error")) {
                    continue;
                }
                count++;
                original += static_cast<qint64>(item.value(QStringLiteral("original_size")).toDouble());
                if (st == QLatin1String("compressed")) {
                    compressed += static_cast<qint64>(item.value(QStringLiteral("compressed_size")).toDouble());
                } else {
                    errors++;
                }
            }
            const qint64 saved = original - compressed;
            const double pct = original > 0 ? (saved * 100.0 / static_cast<double>(original)) : 0.0;
            printOutLn(QStringLiteral("压缩完成。"));
            printOutLn(QStringLiteral("  图片数量：  %1").arg(count));
            printOutLn(QStringLiteral("  原始大小：  %1").arg(toHumanSize(static_cast<double>(original))));
            printOutLn(QStringLiteral("  压缩大小：  %1").arg(toHumanSize(static_cast<double>(compressed))));
            printOutLn(QStringLiteral("  节省：      %1 (%2%)")
                .arg(toHumanSize(static_cast<double>(saved)))
                .arg(QString::number(pct, 'f', 1)));
            if (errors > 0) {
                printOutLn(QStringLiteral("  错误：      %1").arg(errors));
            }
            return EXIT_OK;
        }
        const QJsonObject progress = status.result.value(QStringLiteral("progress")).toObject();
        if (!json) {
            printProgress(QStringLiteral("\r正在压缩... %1/%2")
                .arg(progress.value(QStringLiteral("current")).toInt())
                .arg(progress.value(QStringLiteral("total")).toInt()));
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
    QStringList positional;
    QList<int> ids;
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
                printErrLn(QStringLiteral("错误：缺少 %1 的值。").arg(a));
                return EXIT_BAD_USAGE;
            }
            const QString key = valueFlags.value(a);
            const QString value = args.at(++i);
            if (key == QLatin1String("quality")) {
                bool okNum = false;
                const int q = value.toInt(&okNum);
                if (!okNum) {
                    printErrLn(QStringLiteral("错误：无效的质量值：%1").arg(value));
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
            printErrLn(QStringLiteral("错误：未知选项：%1").arg(a));
            return EXIT_BAD_USAGE;
        }
        positional << a;
    }

    // Consistent with `select`: a pure number is an image ID, anything else is
    // a path. So `compress 4 5 6` compresses images 4, 5 and 6, while
    // `compress ./a.jpg` (or a folder) imports and compresses those paths.
    splitIdsAndPaths(positional, ids, paths);

    if (dryRun) {
        QJsonObject out;
        out[QStringLiteral("dry_run")] = true;
        out[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
        if (!ids.isEmpty()) {
            QJsonArray idArr;
            for (int id : ids) {
                idArr.append(id);
            }
            out[QStringLiteral("ids")] = idArr;
        }
        out[QStringLiteral("options")] = options;
        if (json) {
            printJsonIfNeeded({ true, {}, {}, out }, true);
        } else {
            if (!ids.isEmpty()) {
                QStringList idStrs;
                for (int id : ids) {
                    idStrs << QString::number(id);
                }
                printOutLn(QStringLiteral("预览：将压缩 ID：%1（共 %2 张）。")
                    .arg(idStrs.join(QLatin1String(", ")))
                    .arg(ids.size()));
            } else if (!paths.isEmpty()) {
                printOutLn(QStringLiteral("预览：将处理 %1 个路径。").arg(paths.size()));
            } else {
                printOutLn(QStringLiteral("预览：将压缩当前选中的图片。"));
            }
            printOutLn(QStringLiteral("使用选项："));
            if (options.isEmpty()) {
                printOutLn(QStringLiteral("  使用界面当前选项。"));
            } else {
                printOptionsGrouped(options);
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
                printErrLn(QStringLiteral("导入路径失败：%1").arg(importResp.message));
            }
            return finishResponse(importResp, json);
        }
    }
    if (!options.isEmpty()) {
        const CliResponse setResp = sendWithAutoLaunch(QStringLiteral("options.set"),
            { { QStringLiteral("options"), options } }, autoLaunch);
        if (!setResp.ok) {
            if (!json) {
                printErrLn(QStringLiteral("设置选项失败：%1").arg(setResp.message));
            }
            return finishResponse(setResp, json);
        }
        const QJsonArray unknown = setResp.result.value(QStringLiteral("unknown_keys")).toArray();
        for (const QJsonValue& k : unknown) {
            printErrLn(QStringLiteral("警告：忽略无效选项 '%1'（键不存在或值非法）。").arg(k.toString()));
        }
    }

    QJsonObject params;
    if (onlyFailed) {
        params[QStringLiteral("only_failed")] = true;
    }
    if (!ids.isEmpty()) {
        QJsonArray idArr;
        for (int id : ids) {
            idArr.append(id);
        }
        params[QStringLiteral("ids")] = idArr;
    }
    if (!paths.isEmpty()) {
        // Compress exactly the given paths (plus anything already in the list
        // that matches them), not the whole list.
        params[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
    }
    const CliResponse compressResp = sendWithAutoLaunch(QStringLiteral("compress"), params, autoLaunch);
    if (!compressResp.ok) {
        if (!json) {
            printErrLn(QStringLiteral("错误：%1").arg(compressResp.message));
        }
        return finishResponse(compressResp, json);
    }

    if (wait) {
        return waitAndPrintSummary(json);
    }

    printJsonIfNeeded(compressResp, json);
    if (!json) {
        const int total = compressResp.result.value(QStringLiteral("total")).toInt();
        if (total > 0) {
            printOutLn(QStringLiteral("压缩已开始：%1 张图片。用 'caesium-cli status' 查看进度。").arg(total));
        } else {
            printOutLn(QStringLiteral("压缩已开始。用 'caesium-cli status' 查看进度。"));
        }
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
            printProgress(QStringLiteral("\r正在压缩... %1/%2")
                .arg(progress.value(QStringLiteral("current")).toInt())
                .arg(progress.value(QStringLiteral("total")).toInt()));
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
        printErrLn(QStringLiteral("用法：caesium path status | add | remove"));
        return EXIT_BAD_USAGE;
    }
    const QString sub = args.takeFirst();

    if (sub == QLatin1String("status")) {
        if (!CliPathConfig::isSupported()) {
            if (json) {
                printf("{\"ok\":true,\"result\":{\"supported\":false}}\n");
            } else {
                printOutLn(QStringLiteral("PATH 管理仅适用于 Windows。"));
            }
            return EXIT_OK;
        }
        const bool configured = CliPathConfig::isConfigured();
        if (json) {
            QJsonObject result;
            result[QStringLiteral("supported")] = true;
            result[QStringLiteral("configured")] = configured;
            result[QStringLiteral("bin_dir")] = CliPathConfig::binDir();
            result[QStringLiteral("wrapper")] = CliPathConfig::cliWrapperPath();
            result[QStringLiteral("folder")] = CliPathConfig::cliFolder();
            QJsonObject out;
            out[QStringLiteral("ok")] = true;
            out[QStringLiteral("result")] = result;
            writeJsonOutput(out);
        } else {
            printOutLn(CliPathConfig::cliFolder());
            if (configured) {
                printOutLn(QStringLiteral("caesium-cli 已在您的 PATH 中。"));
            } else {
                printOutLn(QStringLiteral("caesium-cli 不在您的 PATH 中。"));
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
            result[QStringLiteral("bin_dir")] = CliPathConfig::binDir();
            result[QStringLiteral("folder")] = CliPathConfig::cliFolder();
            QJsonObject out;
            out[QStringLiteral("ok")] = true;
            out[QStringLiteral("result")] = result;
            writeJsonOutput(out);
        } else if (ok) {
            printOutLn(sub == QLatin1String("add")
                ? QStringLiteral("已将命令文件夹添加到您的 PATH。软件移动位置后会自动刷新，不会残留旧条目。")
                : QStringLiteral("已从您的 PATH 中移除 caesium-cli 相关条目。"));
            printOutLn(QStringLiteral("重新打开终端（或开一个新的）后生效。"));
        } else {
            printErrLn(QStringLiteral("错误：无法修改 PATH。"));
        }
        return ok ? EXIT_OK : EXIT_OPERATION_ERROR;
    }

    printErrLn(QStringLiteral("用法：caesium path status | add | remove"));
    return EXIT_BAD_USAGE;
}

int runSettings(QStringList args, bool json, bool autoLaunch)
{    if (args.isEmpty()) {
        printErrLn(QStringLiteral("用法：caesium settings get [key] | caesium settings set <key> <value>"));
        return EXIT_BAD_USAGE;
    }
    const QString sub = args.takeFirst();
    if (sub == QLatin1String("get")) {
        QJsonObject params;
        if (!args.isEmpty()) {
            params[QStringLiteral("key")] = args.first();
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("settings.get"), params, autoLaunch);
        if (!json && resp.ok) {
            if (args.isEmpty()) {
                const QJsonObject options = resp.result.value(QStringLiteral("options")).toObject();
                for (auto it = options.begin(); it != options.end(); ++it) {
                    printOutLn(QStringLiteral("%1 = %2").arg(it.key(), jsonValueToString(it.value())));
                }
            } else {
                printOutLn(jsonValueToString(resp.result.value(QStringLiteral("value"))));
            }
        }
        return finishResponse(resp, json);
    }
    if (sub == QLatin1String("set")) {
        // PowerShell drops an empty "" argument when calling the .cmd
        // wrapper, so `settings set output_suffix ""` arrives here as a lone
        // trailing key without a value. Interpret a trailing key with no value
        // as "set to empty string" so the intended command still works.
        if (args.size() >= 1 && (args.size() % 2) != 0) {
            args << QString();
        }
        if (args.size() < 2 || (args.size() % 2) != 0) {
            printErrLn(QStringLiteral("用法：caesium settings set <key> <value> [<key> <value> ...]"));
            return EXIT_BAD_USAGE;
        }
        QJsonObject options;
        for (int i = 0; i + 1 < args.size(); i += 2) {
            options[args.at(i)] = args.at(i + 1);
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("settings.set"),
            { { QStringLiteral("options"), options } }, autoLaunch);
        if (resp.ok && !json) {
            const QJsonArray unknown = resp.result.value(QStringLiteral("unknown_keys")).toArray();
            QSet<QString> badKeys;
            for (const QJsonValue& k : unknown) {
                badKeys.insert(k.toString());
            }
            for (auto it = options.begin(); it != options.end(); ++it) {
                if (badKeys.contains(it.key())) {
                    continue;
                }
                if (it.value().toString().isEmpty()) {
                    printOutLn(QStringLiteral("选项 '%1' 已设置为空。").arg(it.key()));
                } else {
                    printOutLn(QStringLiteral("选项 '%1' 已设置（%2）。").arg(it.key(), it.value().toString()));
                }
            }
            for (const QJsonValue& k : unknown) {
                printErrLn(QStringLiteral("警告：忽略无效设置 '%1'（键不存在或值非法）。").arg(k.toString()));
            }
            if (!unknown.isEmpty()) {
                return EXIT_OPERATION_ERROR;
            }
        }
        return finishResponse(resp, json);
    }
    printErrLn(QStringLiteral("错误：未知的 settings 子命令：%1").arg(sub));
    return EXIT_BAD_USAGE;
}

#ifdef _WIN32

// Command names that can be tab-completed in the interactive shell.
QStringList shellCommandNames()
{
    return {
        QStringLiteral("add"), QStringLiteral("remove"), QStringLiteral("clear"),
        QStringLiteral("list"), QStringLiteral("select"), QStringLiteral("deselect"),
        QStringLiteral("restore"), QStringLiteral("status"), QStringLiteral("preview"),
        QStringLiteral("compress"), QStringLiteral("cancel"), QStringLiteral("settings"),
        QStringLiteral("path"), QStringLiteral("show"), QStringLiteral("quit"),
        QStringLiteral("shell"), QStringLiteral("help"), QStringLiteral("exit"),
    };
}

// Options that can be tab-completed in the interactive shell.
QStringList shellOptionNames()
{
    return {
        QStringLiteral("--json"), QStringLiteral("--no-launch"), QStringLiteral("--wait"),
        QStringLiteral("--only-failed"), QStringLiteral("--dry-run"),
        QStringLiteral("--format"), QStringLiteral("-f"), QStringLiteral("--quality"),
        QStringLiteral("-q"), QStringLiteral("--jpeg-quality"), QStringLiteral("--png-quality"),
        QStringLiteral("--webp-quality"), QStringLiteral("--output"), QStringLiteral("-o"),
        QStringLiteral("--suffix"), QStringLiteral("--fit"), QStringLiteral("--mode"),
        QStringLiteral("--width"), QStringLiteral("--height"), QStringLiteral("--edge"),
        QStringLiteral("--max-output-size"), QStringLiteral("--max-output-size-unit"),
        QStringLiteral("--lossless"), QStringLiteral("--keep-metadata"),
        QStringLiteral("--keep-structure"), QStringLiteral("--keep-dates"),
        QStringLiteral("--same-folder-as-input"), QStringLiteral("--skip-if-bigger"),
        QStringLiteral("--cache-originals"), QStringLiteral("--delete-original-format-file"),
        QStringLiteral("--keep-aspect-ratio"), QStringLiteral("--do-not-enlarge"),
        QStringLiteral("--jpeg-progressive"),
    };
}

// Reads one interactive line with _getwch(). Supports:
//  - Up / Down          history navigation
//  - Left / Right       cursor movement
//  - Home / End         jump to start / end of the line
//  - Backspace / Delete
//  - Tab                completes command names / option names
// The prompt and the whole line are redrawn after every change.
QString readLineInteractive(const QString& prompt, QStringList& history, int& historyPos)
{
    QString line;
    int cursor = 0;

    auto redraw = [&]() {
        // \r -> column 0, print prompt + line, clear to end of line, then
        // re-print the prefix to restore the cursor position.
        printOut(QStringLiteral("\r%1%2\x1b[K").arg(prompt, line));
        if (cursor > 0) {
            printOut(line.left(cursor));
        }
        fflush(stdout);
    };

    auto complete = [&](const QString& prefix, const QStringList& candidates) -> QString {
        QStringList matches;
        for (const QString& c : candidates) {
            if (prefix.isEmpty() || c.startsWith(prefix, Qt::CaseInsensitive)) {
                matches << c;
            }
        }
        if (matches.size() == 1) {
            return matches.first();
        }
        if (matches.size() > 1) {
            printOut(QStringLiteral("\n"));
            for (const QString& m : matches) {
                printOutLn(QStringLiteral("  %1").arg(m));
            }
            redraw();
        }
        return QString();
    };

    redraw();
    for (;;) {
        const wint_t key = _getwch();
        // Extended keys (arrows, Home/End/Delete, F-keys) arrive as 0x00 or
        // 0xE0 followed by a second scan code.
        if (key == 0 || key == 0xE0) {
            const wint_t key2 = _getwch();
            switch (key2) {
            case 0x48: // Up
                if (!history.isEmpty() && historyPos > 0) {
                    --historyPos;
                    line = history.at(historyPos);
                    cursor = line.size();
                    redraw();
                }
                break;
            case 0x50: // Down
                if (historyPos < history.size()) {
                    ++historyPos;
                    line = historyPos == history.size() ? QString() : history.at(historyPos);
                    cursor = line.size();
                    redraw();
                }
                break;
            case 0x4B: // Left
                if (cursor > 0) {
                    --cursor;
                    redraw();
                }
                break;
            case 0x4D: // Right
                if (cursor < line.size()) {
                    ++cursor;
                    redraw();
                }
                break;
            case 0x47: // Home
                cursor = 0;
                redraw();
                break;
            case 0x4F: // End
                cursor = line.size();
                redraw();
                break;
            case 0x53: // Delete
                if (cursor < line.size()) {
                    line.remove(cursor, 1);
                    redraw();
                }
                break;
            default:
                break;
            }
            continue;
        }

        if (key == L'\r' || key == L'\n') {
            printOut(QStringLiteral("\n"));
            if (!line.trimmed().isEmpty()) {
                history << line;
            }
            historyPos = history.size();
            return line;
        }

        if (key == L'\b') {
            if (cursor > 0) {
                line.remove(cursor - 1, 1);
                --cursor;
                redraw();
            }
            continue;
        }

        if (key == L'\t') {
            const bool completingCommand = !line.contains(QLatin1Char(' ')) && !line.contains(QLatin1Char('\t'));
            if (completingCommand) {
                const QString done = complete(line, shellCommandNames());
                if (!done.isEmpty()) {
                    line = done;
                    cursor = line.size();
                    redraw();
                }
            } else {
                const int lastSpace = qMax(line.lastIndexOf(QLatin1Char(' ')), line.lastIndexOf(QLatin1Char('\t')));
                const QString prefix = line.mid(lastSpace + 1);
                const QString done = complete(prefix, shellOptionNames());
                if (!done.isEmpty()) {
                    line = line.left(lastSpace + 1) + done;
                    cursor = line.size();
                    redraw();
                }
            }
            continue;
        }

        line.insert(cursor, QChar(key));
        ++cursor;
        redraw();
    }
}

#endif // _WIN32

int runShell(bool json, bool autoLaunch)
{
    const CliResponse ping = sendWithAutoLaunch(QStringLiteral("ping"), {}, autoLaunch);
    if (!ping.ok) {
        printErrLn(QStringLiteral("Caesium Image Compressor 未在运行，请先启动软件。"));
        return EXIT_NOT_RUNNING;
    }
    printOutLn(QStringLiteral("Caesium CLI 交互模式。输入 'help' 查看命令，'exit' 退出。"));
#ifdef _WIN32
    printOutLn(QStringLiteral("支持 Tab 补全与上下键历史。"));
#endif

    QStringList history;
    int historyPos = 0;

    while (true) {
#ifdef _WIN32
        const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
        const bool interactive = false;
#endif
        QString line;
        if (interactive) {
            line = readLineInteractive(QStringLiteral("caesium> "), history, historyPos);
        } else {
            printOut(QStringLiteral("caesium> "));
            fflush(stdout);
            std::string stdLine;
            if (!std::getline(std::cin, stdLine)) {
                break;
            }
            line = QString::fromStdString(stdLine);
        }
        const QStringList args = QProcess::splitCommand(line);
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
    printOutLn(QStringLiteral("Caesium Image Compressor - 命令行控制运行中的软件。"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("用法："));
    printOutLn(QStringLiteral("  caesium --cli <命令> [选项]"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("命令："));
    printOutLn(QStringLiteral("  add <路径...>              添加文件/文件夹到列表"));
    printOutLn(QStringLiteral("  remove <ID 或路径...>      从列表移除匹配项"));
    printOutLn(QStringLiteral("  clear                      清空整个列表"));
    printOutLn(QStringLiteral("  list                       显示当前列表（含 ID 与选中状态）"));
    printOutLn(QStringLiteral("  select all|none|<ID/路径>  全选 / 取消全选 / 选中指定图片"));
    printOutLn(QStringLiteral("  deselect <ID 或路径...>    取消选中指定图片"));
    printOutLn(QStringLiteral("  restore [ID 或路径...]     还原所有选中 / 指定图片（需缓存原始文件）"));
    printOutLn(QStringLiteral("  status [--wait]            显示进度与每张图片的状态"));
    printOutLn(QStringLiteral("  preview <ID 或路径>        预览某张图压缩后的体积（用当前选项）"));
    printOutLn(QStringLiteral("  compress [ID/路径...] [选项]  压缩指定 ID / 路径，或当前选中图片"));
    printOutLn(QStringLiteral("  cancel                     取消正在进行的压缩"));
    printOutLn(QStringLiteral("  settings get [key]         读取所有设置（含偏好）"));
    printOutLn(QStringLiteral("  settings set <key> <val>... 写入设置，可一次设置多个（如 fit_to long_edge edge 1920）"));
    printOutLn(QStringLiteral("  path status|add|remove     管理 caesium-cli 命令在 PATH 中的状态"));
    printOutLn(QStringLiteral("  show                       把主窗口带到前台"));
    printOutLn(QStringLiteral("  quit                       关闭软件"));
    printOutLn(QStringLiteral("  shell                      与软件交互的会话模式"));
    printOutLn(QStringLiteral("  help                       显示本帮助"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("压缩选项（中文为参数说明，分类与界面一致）："));
    printOutLn(QStringLiteral("  压缩："));
    printOutLn(QStringLiteral("    质量：-q, --quality <0-100>（JPEG/PNG/WebP 同时设置）"));
    printOutLn(QStringLiteral("    JPEG 质量：--jpeg-quality <0-100>"));
    printOutLn(QStringLiteral("    PNG 质量：--png-quality <0-100>"));
    printOutLn(QStringLiteral("    WebP 质量：--webp-quality <0-100>"));
    printOutLn(QStringLiteral("    压缩模式：--mode <quality|size>"));
    printOutLn(QStringLiteral("    无损压缩：--lossless"));
    printOutLn(QStringLiteral("    保留元数据：--keep-metadata"));
    printOutLn(QStringLiteral("    渐进式 JPEG：--jpeg-progressive"));
    printOutLn(QStringLiteral("    PNG 优化级别：--png-optimization-level <1-6>"));
    printOutLn(QStringLiteral("    TIFF 压缩方式：--tiff-method <uncompressed|lzw|deflate|packbits>"));
    printOutLn(QStringLiteral("    TIFF Deflate 级别：--tiff-deflate-level <1-3>"));
    printOutLn(QStringLiteral("    目标输出大小：--max-output-size <n>"));
    printOutLn(QStringLiteral("    大小单位：--max-output-size-unit <bytes|kb|mb|percentage>"));
    printOutLn(QStringLiteral("  缩放："));
    printOutLn(QStringLiteral("    缩放方式：--fit <none|dimensions|percentage|short_edge|long_edge|fixed_width|fixed_height>"));
    printOutLn(QStringLiteral("    宽度 / 高度 / 边长：--width <px> --height <px> --edge <px>"));
    printOutLn(QStringLiteral("    保持宽高比：--keep-aspect-ratio"));
    printOutLn(QStringLiteral("    不放大：--do-not-enlarge"));
    printOutLn(QStringLiteral("  输出："));
    printOutLn(QStringLiteral("    输出格式：--format <jpg|png|webp|tiff|same>"));
    printOutLn(QStringLiteral("    输出文件夹：-o, --output <目录>"));
    printOutLn(QStringLiteral("    输出后缀：--suffix <文本>"));
    printOutLn(QStringLiteral("    输出到输入目录：--same-folder-as-input"));
    printOutLn(QStringLiteral("    保留目录结构：--keep-structure"));
    printOutLn(QStringLiteral("    变大则跳过：--skip-if-bigger"));
    printOutLn(QStringLiteral("    缓存原始文件：--cache-originals（用于还原）"));
    printOutLn(QStringLiteral("    转换后删除原文件：--delete-original-format-file"));
    printOutLn(QStringLiteral("    保留文件日期：--keep-dates"));
    printOutLn(QStringLiteral("  行为："));
    printOutLn(QStringLiteral("    只重试失败项：--only-failed"));
    printOutLn(QStringLiteral("    等待完成并打印汇总：--wait"));
    printOutLn(QStringLiteral("    仅预览不执行：--dry-run"));
    printOutLn(QStringLiteral("  （位置参数是纯数字时视为图片 ID，如 compress 4 5 6）"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("全局选项："));
    printOutLn(QStringLiteral("  --json                     机器可读的 JSON 输出"));
    printOutLn(QStringLiteral("  --no-launch                软件未运行时不要自动启动"));
    printOutLn(QStringLiteral("  --raw <json>               发送原始命令：{\"method\":\"...\",\"params\":{...}}"));
    printOutLn(QStringLiteral("  --version                  显示版本号"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("默认情况下，执行命令前会先启动软件（如果它未在运行）。"));
    printOutLn(QStringLiteral(""));
    printOutLn(QStringLiteral("示例："));
    printOutLn(QStringLiteral("  caesium --cli compress ./photos -q 80 -o ./out --wait"));
    printOutLn(QStringLiteral("  caesium --cli status --json"));
    printOutLn(QStringLiteral("  caesium --cli settings set jpeg_quality 85"));
    printOutLn(QStringLiteral("  caesium --cli settings set fit_to long_edge edge 1920"));
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
        QString raw = args.join(QLatin1Char(' '));
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            // The .cmd wrapper's %* expansion may have stripped the quotes
            // inside the JSON payload; recover it from the raw command line.
            const QString recovered = recoverRawPayload();
            if (!recovered.isEmpty()) {
                QJsonParseError recoveredError;
                const QJsonDocument recoveredDoc = QJsonDocument::fromJson(recovered.toUtf8(), &recoveredError);
                if (recoveredError.error == QJsonParseError::NoError && recoveredDoc.isObject()) {
                    raw = recovered;
                    doc = recoveredDoc;
                    parseError.error = QJsonParseError::NoError;
                }
            }
        }
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            printErrLn(QStringLiteral("错误：--raw 需要合法的 JSON 对象。"));
            return EXIT_BAD_USAGE;
        }
        const QJsonObject req = doc.object();
        const QString method = req.value(QStringLiteral("method")).toString();
        const QJsonObject params = req.value(QStringLiteral("params")).toObject();
        if (method.isEmpty()) {
            printErrLn(QStringLiteral("错误：--raw 需要 'method' 字段。"));
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
            printErrLn(QStringLiteral("用法：caesium add <路径...>"));
            return EXIT_BAD_USAGE;
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("import"),
            { { QStringLiteral("paths"), QJsonArray::fromStringList(args) } }, autoLaunch);
        if (!json && resp.ok) {
            printOutLn(QStringLiteral("已导入 %1 张图片。").arg(resp.result.value(QStringLiteral("added")).toInt()));
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("remove")) {
        if (args.isEmpty()) {
            printErrLn(QStringLiteral("用法：caesium remove <ID 或路径...>"));
            return EXIT_BAD_USAGE;
        }
        QList<int> ids;
        QStringList paths;
        splitIdsAndPaths(args, ids, paths);
        QJsonObject params;
        if (!ids.isEmpty()) {
            QJsonArray idArr;
            for (int id : ids) {
                idArr.append(id);
            }
            params[QStringLiteral("ids")] = idArr;
        }
        if (!paths.isEmpty()) {
            params[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("remove"), params, autoLaunch);
        if (!json && resp.ok) {
            printOutLn(QStringLiteral("已移除 %1 张图片。").arg(resp.result.value(QStringLiteral("removed")).toInt()));
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("clear")) {
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("clear"), {}, autoLaunch);
        if (!json && resp.ok) {
            printOutLn(QStringLiteral("已清空 %1 张图片。").arg(resp.result.value(QStringLiteral("cleared")).toInt()));
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("select") || command == QLatin1String("deselect")) {
        const bool checked = command == QLatin1String("select");
        if (args.isEmpty()) {
            printErrLn(QStringLiteral("用法：caesium %1 all|none|<ID 或路径...>").arg(command));
            return EXIT_BAD_USAGE;
        }
        QJsonObject params;
        const QString first = args.first();
        if (first == QLatin1String("all") || first == QLatin1String("none")) {
            // select all -> all; select none / deselect all -> none;
            // deselect none -> all.
            params[QStringLiteral("all")] = (first == QLatin1String("all")) ? checked : !checked;
            args.removeFirst();
        } else {
            QList<int> ids;
            QStringList paths;
            splitIdsAndPaths(args, ids, paths);
            if (!ids.isEmpty()) {
                QJsonArray idArr;
                for (int id : ids) {
                    idArr.append(id);
                }
                params[QStringLiteral("ids")] = idArr;
            }
            if (!paths.isEmpty()) {
                params[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
            }
            params[QStringLiteral("checked")] = checked;
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("select"), params, autoLaunch);
        if (!json && resp.ok) {
            printOutLn(QStringLiteral("已选 %1 / %2 张。")
                .arg(resp.result.value(QStringLiteral("checked")).toInt())
                .arg(resp.result.value(QStringLiteral("total")).toInt()));
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("preview")) {
        if (args.isEmpty()) {
            printErrLn(QStringLiteral("用法：caesium preview <ID 或路径>"));
            return EXIT_BAD_USAGE;
        }
        QJsonObject params;
        bool okNum = false;
        const int id = args.first().toInt(&okNum);
        if (okNum && id > 0) {
            params[QStringLiteral("id")] = id;
        } else {
            params[QStringLiteral("path")] = args.first();
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("preview"), params, autoLaunch);
        if (!json && resp.ok) {
            printOutLn(QStringLiteral("原始：%1 %2，%3")
                .arg(resp.result.value(QStringLiteral("original_format")).toString(),
                     resp.result.value(QStringLiteral("original_resolution")).toString(),
                     toHumanSize(resp.result.value(QStringLiteral("original_size")).toDouble())));
            printOutLn(QStringLiteral("压缩后：%1 %2，%3（节省 %4 / %5%）")
                .arg(resp.result.value(QStringLiteral("preview_format")).toString(),
                     resp.result.value(QStringLiteral("preview_resolution")).toString(),
                     toHumanSize(resp.result.value(QStringLiteral("preview_size")).toDouble()),
                     toHumanSize(resp.result.value(QStringLiteral("saved")).toDouble()),
                     QString::number(resp.result.value(QStringLiteral("saved_percent")).toDouble(), 'f', 1)));
        }
        return finishResponse(resp, json);
    }

    if (command == QLatin1String("restore")) {
        QJsonObject params;
        if (!args.isEmpty()) {
            QList<int> ids;
            QStringList paths;
            splitIdsAndPaths(args, ids, paths);
            if (!ids.isEmpty()) {
                QJsonArray idArr;
                for (int id : ids) {
                    idArr.append(id);
                }
                params[QStringLiteral("ids")] = idArr;
            }
            if (!paths.isEmpty()) {
                params[QStringLiteral("paths")] = QJsonArray::fromStringList(paths);
            }
        }
        const CliResponse resp = sendWithAutoLaunch(QStringLiteral("restore"), params, autoLaunch);
        if (!json && resp.ok) {
            const int restored = resp.result.value(QStringLiteral("restored")).toInt();
            const int failed = resp.result.value(QStringLiteral("failed")).toInt();
            if (restored > 0 && failed > 0) {
                printOutLn(QStringLiteral("已还原 %1 张，失败 %2 张。").arg(restored).arg(failed));
            } else if (restored > 0) {
                printOutLn(QStringLiteral("已还原 %1 张图片。").arg(restored));
            } else {
                printOutLn(QStringLiteral("没有可还原的图片（需要压缩时开启\"缓存原始文件\"才能还原）。"));
            }
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
            printOutLn(QStringLiteral("压缩已取消。"));
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

    printErrLn(QStringLiteral("未知命令：%1").arg(command));
    printErrLn(QStringLiteral("输入 'caesium --cli help' 查看可用命令。"));
    return EXIT_BAD_USAGE;
}
