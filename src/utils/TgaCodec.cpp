#include "TgaCodec.h"

#include <QFile>
#include <QSaveFile>
#include <QVector>
#include <mutex>

#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace {
constexpr qint64 maxFileBytes = 500LL * 1024 * 1024;
constexpr quint64 maxPixelBytes = 512ULL * 1024 * 1024;
int byte(const QByteArray& data, int offset) { return static_cast<unsigned char>(data[offset]); }
int word(const QByteArray& data, int offset) { return byte(data, offset) | (byte(data, offset + 1) << 8); }

struct Header {
    int width, height, type, depth, descriptor, paletteFirst, paletteCount, paletteDepth;
    int paletteOffset, pixelsOffset;
};

bool load(const QString& path, QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 18 || file.size() > maxFileBytes)
        return false;
    data = file.readAll();
    return data.size() == file.size();
}

// Validate packets before calling stb: its permissive decoder can otherwise
// return an image for truncated input. Also normalize nonzero palette origins,
// which stb treats as a byte offset rather than the first palette index.
bool validate(QByteArray& data, Header& h, QVector<uchar>* alpha = nullptr, bool normalize = false)
{
    if (data.size() < 18) return false;
    const int imageType = byte(data, 2);
    if (imageType != 1 && imageType != 2 && imageType != 3
        && imageType != 9 && imageType != 10 && imageType != 11) return false;
    h = {word(data, 12), word(data, 14), imageType & 7, byte(data, 16), byte(data, 17),
        word(data, 3), word(data, 5), byte(data, 7), 18 + byte(data, 0), 0};
    const quint64 count = quint64(h.width) * h.height;
    if (!h.width || !h.height || count * 4 > maxPixelBytes || (h.descriptor & 0xc0)) return false;
    if (h.type == 1) {
        if (byte(data, 1) != 1 || !h.paletteCount || h.paletteFirst + h.paletteCount > 65536
            || (h.depth != 8 && h.depth != 16)
            || (h.paletteDepth != 15 && h.paletteDepth != 16 && h.paletteDepth != 24 && h.paletteDepth != 32)) return false;
    } else {
        if (byte(data, 1) != 0) return false;
        if (h.type == 2 && h.depth != 15 && h.depth != 16 && h.depth != 24 && h.depth != 32) return false;
        if (h.type == 3 && h.depth != 8 && h.depth != 16) return false;
    }
    const int paletteBytes = (h.paletteDepth + 7) / 8;
    h.pixelsOffset = h.paletteOffset + (h.type == 1 ? h.paletteCount * paletteBytes : 0);
    if (h.pixelsOffset > data.size()) return false;
    const bool alpha16 = (h.descriptor & 15) == 1
        && ((h.type == 2 && h.depth == 16) || (h.type == 1 && h.paletteDepth == 16));
    if (alpha && alpha16) alpha->resize(static_cast<int>(count));
    int position = h.pixelsOffset;
    quint64 pixels = 0;
    const int pixelBytes = (h.depth + 7) / 8;
    while (pixels < count) {
        int length = 1;
        bool repeated = false;
        if (imageType & 8) {
            if (position >= data.size()) return false;
            const int packet = byte(data, position++);
            length = (packet & 127) + 1;
            repeated = packet & 128;
        } else {
            length = static_cast<int>(count - pixels);
        }
        if (pixels + length > count) return false;
        const int samples = repeated ? 1 : length;
        if (qint64(position) + qint64(samples) * pixelBytes > data.size()) return false;
        const bool inspectSamples = h.type == 1 || (alpha && alpha16);
        for (int i = 0; inspectSamples && i < samples; ++i) {
            const int offset = position + i * pixelBytes;
            int colorOffset = offset;
            if (h.type == 1) {
                const int index = h.depth == 8 ? byte(data, offset) : word(data, offset);
                const int normalized = index - h.paletteFirst;
                if (normalized < 0 || normalized >= h.paletteCount) return false;
                colorOffset = h.paletteOffset + normalized * paletteBytes;
                if (normalize) {
                    data[offset] = char(normalized & 255);
                    if (h.depth == 16) data[offset + 1] = char(normalized >> 8);
                }
            }
            if (alpha && alpha16) {
                const uchar a = (word(data, colorOffset) & 0x8000) ? 255 : 0;
                if (repeated) {
                    for (int j = 0; j < length; ++j) (*alpha)[static_cast<int>(pixels) + j] = a;
                } else (*alpha)[static_cast<int>(pixels) + i] = a;
            }
        }
        position += samples * pixelBytes;
        pixels += length;
    }
    if (normalize && h.type == 1) data[3] = data[4] = 0;
    return true;
}

struct Writer { QIODevice* file; bool ok = true; };
void writeBytes(void* context, void* data, int size)
{
    auto* writer = static_cast<Writer*>(context);
    if (writer->ok && writer->file->write(static_cast<const char*>(data), size) != size)
        writer->ok = false;
}
}

QSize TgaCodec::size(const QString& path)
{
    QByteArray data;
    Header h {};
    return load(path, data) && validate(data, h) ? QSize(h.width, h.height) : QSize();
}

bool TgaCodec::hasMetadata(const QString& path)
{
    QByteArray data;
    if (!load(path, data)) return false;
    if (byte(data, 0) != 0) return true; // Image ID
    if (data.size() >= 26 && data.right(18) == QByteArray("TRUEVISION-XFILE.\0", 18)) {
        // Nonzero extension/developer offsets point to metadata whose internal
        // offsets cannot be preserved by simply appending the original footer.
        const int footer = int(data.size()) - 26;
        for (int i = 0; i < 8; ++i) if (byte(data, footer + i)) return true;
    }
    return false;
}

QImage TgaCodec::read(const QString& path, QString* error)
{
    QByteArray data;
    Header h {};
    QVector<uchar> alpha;
    if (!load(path, data) || !validate(data, h, &alpha, true)) {
        if (error) *error = QStringLiteral("Invalid, truncated, unsupported or oversized TGA image");
        return {};
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.constData()),
        int(data.size()), &width, &height, &channels, 4);
    if (!pixels) {
        if (error) *error = QString::fromUtf8(stbi_failure_reason());
        return {};
    }
    // Own the result independently of stb's allocation.
    QImage image = QImage(pixels, width, height, width * 4, QImage::Format_RGBA8888).copy();
    stbi_image_free(pixels);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("Cannot allocate TGA image");
        return {};
    }
    // stb ignores the 1-bit alpha in 16-bit truecolor/palette entries.
    for (int i = 0; i < alpha.size(); ++i) {
        int y = i / width;
        if (!(h.descriptor & 0x20)) y = height - 1 - y;
        image.scanLine(y)[(i % width) * 4 + 3] = alpha[i];
    }
    // stb handles the vertical origin but not the horizontal origin.
    if (h.descriptor & 0x10) image = image.mirrored(true, false);
    if (channels != 2 && channels != 4 && alpha.isEmpty()) image = image.convertToFormat(QImage::Format_RGB888);
    return image;
}

bool TgaCodec::write(const QString& path, const QImage& source, QString* error, bool rle)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (!write(file, source, error, rle)) return false;
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool TgaCodec::write(QIODevice& file, const QImage& source, QString* error, bool rle)
{
    if (source.isNull() || source.width() > 65535 || source.height() > 65535
        || quint64(source.width()) * source.height() * 4 > maxPixelBytes) {
        if (error) *error = QStringLiteral("Invalid or oversized TGA output dimensions");
        return false;
    }
    // RGBA8888 has tightly packed rows; RGB888 can have Qt padding. Preserve
    // opaque RGB input as 24-bit, and every alpha value of RGBA input as 32-bit.
    const bool alpha = source.hasAlphaChannel();
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("Cannot allocate TGA output image");
        return false;
    }
    QByteArray rgb;
    const void* pixels = image.constBits();
    if (!alpha) {
        rgb.resize(source.width() * source.height() * 3);
        for (int y = 0; y < image.height(); ++y) {
            const uchar* row = image.constScanLine(y);
            char* out = rgb.data() + y * image.width() * 3;
            for (int x = 0; x < image.width(); ++x)
                for (int c = 0; c < 3; ++c) out[x * 3 + c] = char(row[x * 4 + c]);
        }
        pixels = rgb.constData();
    }
    if (!file.isWritable()) {
        if (error) *error = file.errorString();
        return false;
    }
    Writer writer { &file };
    // stb exposes compression as a global, so changing it and encoding must
    // share a lock. Pixel preparation and decoding can still run in parallel.
    static std::mutex writerMutex;
    const std::lock_guard<std::mutex> lock(writerMutex);
    stbi_write_tga_with_rle = rle ? 1 : 0;
    if (!stbi_write_tga_to_func(writeBytes, &writer, image.width(), image.height(), alpha ? 4 : 3, pixels)
        || !writer.ok) {
        if (error) *error = QStringLiteral("Cannot write TGA output: ") + file.errorString();
        return false;
    }
    return true;
}
