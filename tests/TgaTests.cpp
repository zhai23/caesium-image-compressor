#include <QtTest>
#include <QtConcurrent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "models/CImage.h"
#include "services/Importer.h"
#include "utils/ImageIO.h"
#include "utils/TgaCodec.h"
#include "exceptions/ImageNotSupportedException.h"

namespace {
void putWord(QByteArray& data, int offset, int value)
{
    data[offset] = char(value & 255);
    data[offset + 1] = char(value >> 8);
}
QByteArray header(int type, int width, int height, int depth, int descriptor = 0x20)
{
    QByteArray data(18, '\0');
    data[2] = char(type);
    putWord(data, 12, width);
    putWord(data, 14, height);
    data[16] = char(depth);
    data[17] = char(descriptor);
    return data;
}
bool save(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
QImage sample(bool alpha = true)
{
    QImage image(16, 8, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(y * 29, 35, 255 - y * 17, alpha ? (y * 37) % 256 : 255));
    return image;
}
// Independent uncompressed fixture writer, including all four image origins.
QByteArray rawTga(const QImage& image, int origin = 0x20)
{
    const bool alpha = image.hasAlphaChannel();
    QByteArray data = header(2, image.width(), image.height(), alpha ? 32 : 24, origin | (alpha ? 8 : 0));
    for (int row = 0; row < image.height(); ++row) {
        const int y = (origin & 0x20) ? row : image.height() - 1 - row;
        for (int col = 0; col < image.width(); ++col) {
            const int x = (origin & 0x10) ? image.width() - 1 - col : col;
            const QColor color = image.pixelColor(x, y);
            data.append(char(color.blue())).append(char(color.green())).append(char(color.red()));
            if (alpha) data.append(char(color.alpha()));
        }
    }
    return data;
}
bool equalPixels(const QImage& a, const QImage& b)
{
    return !a.isNull() && a.convertToFormat(QImage::Format_RGBA8888) == b.convertToFormat(QImage::Format_RGBA8888);
}
CompressionOptions options(const QString& output)
{
    CompressionOptions opt {};
    opt.outputPath = output;
    opt.suffix = "_out";
    opt.lossless = true;
    opt.jpegQuality = 95;
    opt.jpegChromaSubsampling = 444;
    opt.pngQuality = 100;
    opt.pngOptimizationLevel = 2;
    opt.webpQuality = 100;
    opt.tiffMethod = 1;
    opt.tiffDeflateLevel = 6;
    opt.transparencyFillColor = "#FFFFFF";
    opt.maxOutputSize = {MAX_OUTPUT_BYTES, 1000000};
    return opt;
}
}

class TgaTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("CaesiumTests");
        QCoreApplication::setApplicationName("TgaTests");
    }
    void originsAndAlpha_data()
    {
        QTest::addColumn<int>("origin");
        QTest::addColumn<bool>("alpha");
        for (int origin : {0, 16, 32, 48})
            for (bool alpha : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(origin).arg(alpha))) << origin << alpha;
    }
    void originsAndAlpha()
    {
        QFETCH(int, origin);
        QFETCH(bool, alpha);
        QTemporaryDir dir;
        auto expected = sample(alpha);
        expected.setPixelColor(0, 0, QColor(199, 23, 170, alpha ? 13 : 255));
        const QString path = dir.filePath(QString::fromUtf8("纹理.TGA"));
        QVERIFY(save(path, rawTga(expected, origin)));
        QCOMPARE(ImageIO::format(path), QString("tga"));
        QCOMPARE(ImageIO::size(path), expected.size());
        QVERIFY(equalPixels(ImageIO::read(path), expected));
        const QString output = dir.filePath("rle-cache-without-extension");
        QVERIFY(TgaCodec::write(output, ImageIO::read(path)));
        QCOMPARE(int(uchar(bytes(output)[2])), 10);
        QCOMPARE(int(uchar(bytes(output)[16])), alpha ? 32 : 24);
        QVERIFY(equalPixels(ImageIO::read(output), expected));
    }
    void grayscalePaletteAnd16BitAlpha()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("variants.tga");
        auto data = header(11, 3, 1, 8);
        data.append(char(0x82)).append(char(125));
        QVERIFY(save(path, data));
        QCOMPARE(TgaCodec::read(path).pixelColor(2, 0), QColor(125, 125, 125));

        data = header(3, 1, 1, 16, 0x28);
        data.append(char(80)).append(char(40));
        QVERIFY(save(path, data));
        QCOMPARE(TgaCodec::read(path).pixelColor(0, 0), QColor(80, 80, 80, 40));

        data = header(9, 2, 1, 8, 0x28);
        data[1] = 1;
        putWord(data, 3, 5);
        putWord(data, 5, 2);
        data[7] = 32;
        data.append(QByteArray::fromHex("0000ff40ff0000ff")); // palette starts at index 5
        data.append(char(1)).append(char(5)).append(char(6)); // raw RLE packet
        QVERIFY(save(path, data));
        auto image = TgaCodec::read(path);
        QCOMPARE(image.pixelColor(0, 0), QColor(255, 0, 0, 64));
        QCOMPARE(image.pixelColor(1, 0), QColor(0, 0, 255, 255));

        data = header(10, 2, 1, 16, 0x31);
        data.append(char(1)).append(QByteArray::fromHex("00fc1f00"));
        QVERIFY(save(path, data));
        image = TgaCodec::read(path);
        QCOMPARE(image.pixelColor(0, 0), QColor(0, 0, 255, 0));
        QCOMPARE(image.pixelColor(1, 0), QColor(255, 0, 0, 255));
    }
    void malformedFiles()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("invalid.tga");
        QList<QByteArray> invalid;
        invalid << QByteArray("bad") << header(2, 65535, 65535, 32)
                << header(2, 4, 4, 24) << (header(10, 1, 1, 24) + QByteArray::fromHex("81000000"))
                << header(2, 1, 1, 24, 0xc0) << header(2, 0, 1, 24);
        for (const auto& data : invalid) {
            QVERIFY(save(path, data));
            QVERIFY(!TgaCodec::size(path).isValid());
            QVERIFY(TgaCodec::read(path).isNull());
            QVERIFY_EXCEPTION_THROWN((void)CImage{path}, ImageNotSupportedException);
        }
    }
    void importAndCompression()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.TGA");
        const auto expected = sample();
        QVERIFY(save(path, rawTga(expected)));
        QVERIFY(Importer::scanDirectory(dir.path(), true).contains(path));
        CImage image(path);
        auto opt = options(dir.filePath("output"));
        QVERIFY(image.preview(opt));
        QVERIFY(equalPixels(ImageIO::read(image.getTemporaryPreviewFullPath()), expected));
        QVERIFY(image.compress(opt));
        QVERIFY(image.getCompressedSize() < image.getOriginalSize());
        QVERIFY(equalPixels(ImageIO::read(image.getCompressedFullPath()), expected));
        QCOMPARE(image.getResolution(), QString("16x8"));
        QFile::remove(image.getTemporaryPreviewFullPath());
    }
    void conversions_data()
    {
        QTest::addColumn<int>("format");
        for (int format : {1, 2, 3, 4, 5}) QTest::newRow(qPrintable(QString::number(format))) << format;
    }
    void conversions()
    {
        QFETCH(int, format);
        QTemporaryDir dir;
        const auto expected = sample();
        const QString path = dir.filePath("texture.tga");
        QVERIFY(save(path, rawTga(expected)));
        CImage image(path);
        auto opt = options(dir.filePath("output"));
        opt.format = format;
        QVERIFY2(image.preview(opt), "Preview conversion failed");
        QVERIFY2(image.compress(opt), qPrintable(image.getFormattedStatus()));
        const QImage result = ImageIO::read(image.getCompressedFullPath());
        QCOMPARE(result.size(), expected.size());
        if (format == 2 || format == 5) QVERIFY(equalPixels(result, expected));
        if (format == 3) {
            // WebP may canonicalize RGB underneath fully transparent pixels.
            for (int y = 0; y < expected.height(); ++y)
                for (int x = 0; x < expected.width(); ++x) {
                    QCOMPARE(result.pixelColor(x, y).alpha(), expected.pixelColor(x, y).alpha());
                    if (expected.pixelColor(x, y).alpha()) QCOMPARE(result.pixelColor(x, y), expected.pixelColor(x, y));
                }
        }
        if (format == 1) {
            QVERIFY(!result.hasAlphaChannel());
            const QColor white = result.pixelColor(8, 0);
            QVERIFY(white.red() > 240 && white.green() > 240 && white.blue() > 240);
        }
        QFile::remove(image.getTemporaryPreviewFullPath());
    }
    void pngToTgaAndResize()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.png");
        QVERIFY(sample().save(path));
        CImage image(path);
        auto opt = options(dir.filePath("output"));
        opt.format = 5;
        opt.resize = true;
        opt.fitTo = FIXED_WIDTH;
        opt.width = 8;
        QVERIFY(image.compress(opt));
        QCOMPARE(ImageIO::size(image.getCompressedFullPath()), QSize(8, 4));
        QVERIFY(image.preview(opt));
        QVERIFY(equalPixels(ImageIO::read(image.getTemporaryPreviewFullPath()), ImageIO::read(image.getCompressedFullPath())));
        QFile::remove(image.getTemporaryPreviewFullPath());
    }
    void sizeLimitDoesNotReplaceOutput()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const QByteArray original = rawTga(sample());
        QVERIFY(save(path, original));
        CImage image(path);
        auto opt = options(dir.path());
        opt.compressionMode = SIZE;
        opt.maxOutputSize.maxOutputSize = 1;
        const QString output = dir.filePath("texture_out.tga");
        QVERIFY(save(output, "existing output"));
        QVERIFY(!image.compress(opt));
        QCOMPARE(bytes(path), original);
        QCOMPARE(bytes(output), QByteArray("existing output"));
        QVERIFY(!image.preview(opt));
        opt.maxOutputSize.maxOutputSize = 10000;
        QVERIFY(image.compress(opt));
    }
    void restoreAndMeta()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const QByteArray original = rawTga(sample());
        const QByteArray meta = "{\"uuid\":\"test-uuid\",\"files\":[\".tga\"]}";
        QVERIFY(save(path, original));
        QVERIFY(save(path + ".meta", meta));
        CImage image(path);
        auto opt = options(dir.path());
        opt.format = 2;
        opt.suffix.clear();
        opt.sameFolderAsInput = true;
        opt.cacheOriginals = true;
        opt.deleteOriginalFormatFile = true;
        opt.syncMetaFile = true;
        QVERIFY(image.compress(opt));
        QVERIFY(!QFile::exists(path));
        QVERIFY(bytes(dir.filePath("texture.png.meta")).contains("test-uuid"));
        QVERIFY(image.restoreFromCache());
        QCOMPARE(bytes(path), original);
        QCOMPARE(bytes(path + ".meta"), meta);
        QVERIFY(!QFile::exists(dir.filePath("texture.png")));
        image.removeCachedOriginal();
    }
    void restoreAndSpineAtlas()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const QByteArray original = rawTga(sample());
        const QByteArray atlas = "texture.tga\r\nsize:1102,3976\r\nfilter:Linear,Linear\r\n0\r\nbounds:1039,264,37,43\r\n";
        const QByteArray syncedAtlas = "texture.png\r\nsize:1102,3976\r\nfilter:Linear,Linear\r\n0\r\nbounds:1039,264,37,43\r\n";
        // Same-name atlas in the ".atlas.txt" flavour, with the page attributes
        // glued right after the image name on the first line.
        const QByteArray gluedAtlas = "texture.tgasize:1102,3976\nfilter:Linear,Linear\n";
        const QByteArray syncedGluedAtlas = "texture.pngsize:1102,3976\nfilter:Linear,Linear\n";
        QVERIFY(save(path, original));
        QVERIFY(save(dir.filePath("texture.atlas"), atlas));
        QVERIFY(save(dir.filePath("texture.atlas.txt"), gluedAtlas));
        CImage image(path);
        auto opt = options(dir.path());
        opt.format = 2;
        opt.suffix.clear();
        opt.sameFolderAsInput = true;
        opt.cacheOriginals = true;
        opt.deleteOriginalFormatFile = true;
        opt.syncSpineFile = true;
        QVERIFY(image.compress(opt));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(bytes(dir.filePath("texture.atlas")), syncedAtlas);
        QCOMPARE(bytes(dir.filePath("texture.atlas.txt")), syncedGluedAtlas);
        QVERIFY(image.restoreFromCache());
        QCOMPARE(bytes(path), original);
        QCOMPARE(bytes(dir.filePath("texture.atlas")), atlas);
        QCOMPARE(bytes(dir.filePath("texture.atlas.txt")), gluedAtlas);
        QVERIFY(!QFile::exists(dir.filePath("texture.png")));
        image.removeCachedOriginal();
    }
    void spineAtlasFollowsOnlyItsOwnImage()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const QByteArray original = rawTga(sample());
        const QByteArray atlas = "other.png\nsize:1102,3976\n";
        QVERIFY(save(path, original));
        QVERIFY(save(dir.filePath("texture.atlas"), atlas));
        CImage image(path);
        auto opt = options(dir.path());
        opt.format = 2;
        opt.suffix.clear();
        opt.sameFolderAsInput = true;
        opt.cacheOriginals = true;
        opt.deleteOriginalFormatFile = true;
        opt.syncSpineFile = true;
        QVERIFY(image.compress(opt));
        // The atlas does not point at the converted image: it must stay as is.
        QCOMPARE(bytes(dir.filePath("texture.atlas")), atlas);
        QVERIFY(image.restoreFromCache());
        QCOMPARE(bytes(path), original);
        QCOMPARE(bytes(dir.filePath("texture.atlas")), atlas);
        image.removeCachedOriginal();
    }
    void sameFormatRestore()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const auto original = rawTga(sample());
        QVERIFY(save(path, original));
        CImage image(path);
        auto opt = options(dir.path());
        opt.suffix.clear();
        opt.sameFolderAsInput = true;
        opt.cacheOriginals = true;
        QVERIFY(image.compress(opt));
        QVERIFY(bytes(path).size() < original.size());
        QVERIFY(image.restoreFromCache());
        QCOMPARE(bytes(path), original);
        image.removeCachedOriginal();
    }
    void metadataIsNotSilentlyDropped()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("metadata.tga");
        auto data = rawTga(sample());
        data[0] = 4;
        data.insert(18, "info");
        QVERIFY(save(path, data));
        CImage image(path);
        auto opt = options(dir.filePath("output"));
        opt.keepMetadata = true;
        QVERIFY(!image.compress(opt));
        QCOMPARE(bytes(path), data);
        opt.keepMetadata = false;
        QVERIFY(image.compress(opt));
    }
    void concurrentWriters()
    {
        QTemporaryDir dir;
        QList<QFuture<bool>> futures;
        for (int i = 0; i < 8; ++i) {
            const QString path = dir.filePath(QString::number(i) + ".tga");
            futures.append(QtConcurrent::run([path, i]() {
                const QImage expected = sample(i % 2);
                const bool rle = (i / 2) % 2;
                return TgaCodec::write(path, expected, nullptr, rle)
                    && uchar(bytes(path)[2]) == (rle ? 10 : 2)
                    && equalPixels(TgaCodec::read(path), expected);
            }));
        }
        for (auto& future : futures) QVERIFY(future.result());
    }
    void rgbPaddingAndSkipIfBigger()
    {
        QTemporaryDir dir;
        QImage expected(3, 2, QImage::Format_RGB888);
        expected.fill(QColor(10, 20, 30));
        expected.setPixelColor(2, 1, QColor(40, 50, 60));
        const QString path = dir.filePath("odd.tga");
        QVERIFY(TgaCodec::write(path, expected));
        QVERIFY(equalPixels(TgaCodec::read(path), expected));

        QImage tiny(1, 1, QImage::Format_RGB888);
        tiny.fill(Qt::red);
        const QString source = dir.filePath("tiny.tga");
        const auto original = rawTga(tiny);
        QVERIFY(save(source, original));
        CImage image(source);
        auto opt = options(dir.path());
        opt.suffix.clear();
        opt.sameFolderAsInput = true;
        opt.skipIfBigger = true;
        QVERIFY(image.compress(opt));
        QCOMPARE(bytes(source), original);
        QCOMPARE(image.getStatus(), CImageStatus::WARNING);
    }
    void uncompressedPreviewWithoutExtension_data()
    {
        QTest::addColumn<int>("edge");
        QTest::addColumn<bool>("alpha");
        for (int edge : {256, 2048})
            for (bool alpha : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(edge).arg(alpha))) << edge << alpha;
    }
    void uncompressedPreviewWithoutExtension()
    {
        QFETCH(int, edge);
        QFETCH(bool, alpha);
        QTemporaryDir dir;
        QImage expected(edge, edge, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        expected.fill(QColor(199, 23, 170, alpha ? 73 : 255));
        const QString path = dir.filePath("preview-cache-without-extension");
        QVERIFY(TgaCodec::write(path, expected, nullptr, false));
        // Some uncompressed TGA headers are also accepted by Qt's ICO probe.
        QCOMPARE(ImageIO::format(path), QString("tga"));
        QCOMPARE(ImageIO::size(path), expected.size());
        QVERIFY(equalPixels(ImageIO::read(path), expected));
    }
    void iconAndCursorDetection()
    {
        QTemporaryDir dir;
        QImage expected = sample();
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(expected.save(&buffer, "PNG"));
        for (int type : {1, 2}) {
            QByteArray icon(22, '\0');
            putWord(icon, 2, type);
            putWord(icon, 4, 1);
            icon[6] = char(expected.width());
            icon[7] = char(expected.height());
            putWord(icon, 10, type == 1 ? 1 : 0);
            putWord(icon, 12, type == 1 ? 32 : 0);
            for (int i = 0; i < 4; ++i) icon[14 + i] = char(quint32(png.size()) >> (i * 8));
            icon[18] = 22;
            icon.append(png);
            const QString path = dir.filePath("icon-without-extension");
            QVERIFY(save(path, icon));
            QVERIFY(ImageIO::format(path) != "tga");
            QCOMPARE(ImageIO::size(path), expected.size());
            QVERIFY(equalPixels(ImageIO::read(path), expected));
        }
    }
    void compressionMethodAndPreview()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.tga");
        const QImage expected = sample();
        QVERIFY(save(path, rawTga(expected)));
        CImage image(path);
        auto opt = options(dir.filePath("output"));
        QVERIFY(opt.tgaRle);
        opt.tgaRle = false;
        QVERIFY(image.preview(opt));
        QCOMPARE(uchar(bytes(image.getTemporaryPreviewFullPath())[2]), uchar(2));
        QVERIFY(equalPixels(ImageIO::read(image.getTemporaryPreviewFullPath()), expected));
        QVERIFY(image.compress(opt));
        QCOMPARE(uchar(bytes(image.getCompressedFullPath())[2]), uchar(2));
        QCOMPARE(image.getCompressedSize(), size_t(18 + expected.width() * expected.height() * 4));
        QVERIFY(equalPixels(ImageIO::read(image.getCompressedFullPath()), expected));
        opt.tgaRle = true;
        opt.suffix = "_rle";
        QVERIFY(image.preview(opt));
        QCOMPARE(uchar(bytes(image.getTemporaryPreviewFullPath())[2]), uchar(10));
        QVERIFY(image.compress(opt));
        QVERIFY(image.getCompressedSize() < image.getOriginalSize());
        QVERIFY(equalPixels(ImageIO::read(image.getCompressedFullPath()), expected));
        QFile::remove(image.getTemporaryPreviewFullPath());
    }
};

QTEST_MAIN(TgaTests)
#include "TgaTests.moc"
