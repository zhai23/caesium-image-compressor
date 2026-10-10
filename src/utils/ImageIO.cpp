#include "ImageIO.h"
#include "TgaCodec.h"
#include "Utils.h"
#include <QFileInfo>
#include <QImageReader>

QString ImageIO::format(const QString& path)
{
    if (QFileInfo(path).suffix().compare("tga", Qt::CaseInsensitive) == 0)
        return TgaCodec::size(path).isValid() ? QStringLiteral("tga") : QString();
    const QString format = QString::fromLatin1(QImageReader::imageFormat(path)).toLower();
    // Qt's icon probe also accepts some uncompressed TGA headers (notably
    // dimensions divisible by 256). Extensionless preview/original caches
    // must use TGA when its full header and pixel payload validate, instead
    // of passing these files to the ICO decoder and getting an empty image.
    if ((format == "ico" || format == "cur") && TgaCodec::size(path).isValid())
        return QStringLiteral("tga");
    if (!format.isEmpty() && format != "tga") return format == "jpeg" ? QStringLiteral("jpg") : format;
    // Preview cache files have no extension. TGA has no reliable magic bytes,
    // so only try its strictly validated decoder after other formats fail.
    return TgaCodec::size(path).isValid() ? QStringLiteral("tga") : QString();
}

QSize ImageIO::size(const QString& path)
{
    if (format(path) == "tga") return TgaCodec::size(path);
    QImageReader reader(path);
    return getSizeWithMetadata(&reader);
}

QImage ImageIO::read(const QString& path, bool autoTransform)
{
    if (format(path) == "tga") return TgaCodec::read(path);
    QImageReader reader(path);
    reader.setAutoTransform(autoTransform);
    return reader.read();
}
