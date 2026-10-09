#ifndef CIMAGE_H
#define CIMAGE_H

#include "../include/libcaesium.h"
#include "../utils/Utils.h"

class CImage {
    const QList<QByteArray> supportedFormats = { "png", "jpg", "jpeg", "webp", "tiff", "tga" };

public:
    explicit CImage(const QString& path);

    friend bool operator==(const CImage& c1, const CImage& c2);
    friend bool operator!=(const CImage& c1, const CImage& c2);

    QString getFormattedSize() const;
    QString getRichFormattedSize() const;
    QString getResolution() const;
    QString getRichResolution() const;
    QString getFileName() const;
    QString getFullPath() const;
    size_t getOriginalSize() const;
    size_t getCompressedSize() const;
    CImageStatus getStatus() const;
    void setStatus(const CImageStatus& value);
    double getRatio() const;
    QString getFormattedSavedRatio() const;
    QString getRichFormattedSavedRatio() const;
    bool compress(const CompressionOptions& compressionOptions);
    bool preview(const CompressionOptions& compressionOptions) const;
    QString getCompressedFullPath() const;
    QString getTemporaryPreviewFullPath() const;
    QString getPreviewFullPath() const;
    size_t getTotalPixels() const;
    QString getFormattedStatus() const;
    QString getDirectory() const;
    QString getCompressedDirectory() const;
    QString getHashedFullPath() const;
    QString getFormat() const;
    CCSParameters getCSParameters(const CompressionOptions& compressionOptions) const;

    QString getCachedOriginalPath() const;
    bool ensureCachedOriginal() const;
    QString getCompressionSourcePath(bool cacheOriginals) const;
    void removeCachedOriginal() const;
    // Restore the image to its uncompressed original by copying the cached
    // original back to its location and removing the converted output.
    bool restoreFromCache();

    // Session-local numeric ID (1, 2, 3, ...) assigned at import time, so the
    // CLI can reference images by ID instead of by full path.
    int getCliId() const;
    void setCliId(int id);

private:
    bool writeTgaOutput(const QString& sourcePath, const QString& outputPath,
        const CompressionOptions& options, QString& error) const;
    CImageStatus status;
    QString fullPath;
    QString fileName;
    QString compressedFullPath;
    QString directory;
    QString compressedDirectory;
    QString additionalInfo;
    QString hashedFullPath;
    QString extension;
    QString format;
    int cliId = 0;

    size_t size;
    size_t compressedSize;

    int width;
    int height;
    int compressedWidth;
    int compressedHeight;

    void setCompressedInfo(const QFileInfo& fileInfo);
    static void setFileDates(const QFileInfo& fileInfo, FileDatesOutputOption datesMap, const FileDates& inputFileDates);
    static size_t getMaxOutputSizeInBytes(MaxOutputSize maxOutputSize, size_t originalSize);
    // Prepare a source QImage for saving to a target format: normalize palette
    // images and, for formats without alpha (JPG), composite transparent pixels
    // onto the given fill color so they don't become garbage.
    static QImage prepareImageForConversion(const QImage& source, int targetFormatIndex, const QString& fillColorHex);

    // Rename an adjacent Cocos Creator .meta file so it follows a format
    // conversion (e.g. 1.png.meta -> 1.jpg.meta) and update the format entry
    // recorded inside it (the top-level "files" list). sourceImagePath /
    // outputImagePath are the paths of the old and the new image files.
    static bool syncMetaFileForConversion(const QString& sourceImagePath, const QString& outputImagePath);
};

#endif // CIMAGE_H
