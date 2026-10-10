#pragma once

#include <QImage>
#include <QIODevice>
#include <QString>

// File I/O uses Qt so Unicode paths work on every platform. Pixel coding is stb.
namespace TgaCodec {
QSize size(const QString& path);
bool hasMetadata(const QString& path);
QImage read(const QString& path, QString* error = nullptr);
bool write(const QString& path, const QImage& image, QString* error = nullptr, bool rle = true);
bool write(QIODevice& device, const QImage& image, QString* error = nullptr, bool rle = true);
}
