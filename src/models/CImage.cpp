#include "CImage.h"
#include "../include/libcaesium.h"

#include "./exceptions/ImageNotSupportedException.h"
#include "exceptions/ImageTooBigException.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <cmath>

CImage::CImage(const QString& path)
{
    QFileInfo fileInfo = QFileInfo(path);
    auto* imageReader = new QImageReader();
    imageReader->setAutoDetectImageFormat(true);
    imageReader->setAutoTransform(true);
    imageReader->setFileName(path);
    this->format = imageReader->format().toLower();
    if (this->format == "jpeg") {
        this->format = "jpg";
    }

    if (!supportedFormats.contains(format)) {
        delete imageReader;
        throw ImageNotSupportedException();
    }

    this->extension = fileInfo.suffix();
    this->size = fileInfo.size();

    if (this->size > 500 * 1024 * 1024) {
        throw ImageTooBigException();
    }

    this->fullPath = fileInfo.canonicalFilePath();
    this->directory = fileInfo.canonicalPath();
    this->fileName = fileInfo.fileName();
    this->compressedSize = this->size;

    QSize imageSize = getSizeWithMetadata(imageReader);
    this->width = imageSize.width();
    this->height = imageSize.height();

    this->compressedWidth = this->width;
    this->compressedHeight = this->height;

    this->hashedFullPath = hashString(this->fullPath, QCryptographicHash::Sha256);

    this->status = CImageStatus::UNCOMPRESSED;

    delete imageReader;
}

bool operator==(const CImage& c1, const CImage& c2)
{
    return (c1.fullPath == c2.fullPath);
}

bool operator!=(const CImage& c1, const CImage& c2)
{
    return !(c1 == c2);
}

QString CImage::getFormattedSize() const
{
    bool needsFormatting = this->status == CImageStatus::COMPRESSED || this->status == CImageStatus::WARNING;
    size_t s = needsFormatting ? this->compressedSize : this->size;
    return toHumanSize(static_cast<double>(s));
}

QString CImage::getRichFormattedSize() const
{
    bool needsFormatting = this->status == CImageStatus::COMPRESSED || this->status == CImageStatus::WARNING;
    if (needsFormatting && this->size != this->compressedSize) {
        return "<small><s>" + toHumanSize(static_cast<double>(this->size)) + "</s></small> " + toHumanSize(static_cast<double>(this->compressedSize));
    }
    return toHumanSize(static_cast<double>(this->size));
}

QString CImage::getResolution() const
{
    if (this->status == CImageStatus::COMPRESSED || this->status == CImageStatus::WARNING) {
        return QString::number(this->compressedWidth) + "x" + QString::number(this->compressedHeight);
    }
    return QString::number(this->width) + "x" + QString::number(this->height);
}

QString CImage::getRichResolution() const
{
    bool needsFormatting = this->status == CImageStatus::COMPRESSED || this->status == CImageStatus::WARNING;
    if (needsFormatting && (this->width != this->compressedWidth || this->height != this->compressedHeight)) {
        return "<small><s>" + QString::number(this->width) + "x" + QString::number(this->height) + "</s></small> " + QString::number(this->compressedWidth) + "x" + QString::number(this->compressedHeight);
    }
    return QString::number(this->width) + "x" + QString::number(this->height);
}

QString CImage::getFileName() const
{
    return fileName;
}

QString CImage::getFullPath() const
{
    return this->fullPath;
}

int CImage::getCliId() const
{
    return this->cliId;
}

void CImage::setCliId(int id)
{
    this->cliId = id;
}

bool CImage::preview(const CompressionOptions& compressionOptions) const
{
    QString inputFullPath = this->getCompressionSourcePath(compressionOptions.cacheOriginals);
    QFileInfo inputFileInfo(inputFullPath);
    QString outputFullPath = this->getTemporaryPreviewFullPath();
    if (outputFullPath.isEmpty()) {
        return false;
    }
    const QString& outputFormat = getOutputSupportedFormats()[compressionOptions.format];
    bool convert = compressionOptions.format != 0 && this->getFormat().compare(outputFormat, Qt::CaseInsensitive) != 0;
    FileDates inputFileDates = {
        inputFileInfo.fileTime(QFile::FileBirthTime),
        inputFileInfo.fileTime(QFile::FileModificationTime),
        inputFileInfo.fileTime(QFile::FileAccessTime)
    };
    CCSParameters r_parameters = this->getCSParameters(compressionOptions);
    if (convert) {
        QImage imageToBeConverted = QImage(inputFullPath);
        imageToBeConverted = prepareImageForConversion(imageToBeConverted, compressionOptions.format, compressionOptions.transparencyFillColor);
        bool conversionSuccess = imageToBeConverted.save(outputFullPath, getOutputSupportedFormats().at(compressionOptions.format).toLower().toUtf8().constData(), 100);
        if (!conversionSuccess) {
            return false;
        }
        inputFullPath = outputFullPath;
    }
    size_t maxOutputSize = getMaxOutputSizeInBytes(compressionOptions.maxOutputSize, inputFileInfo.size());

    CCSResult result = compressionOptions.compressionMode == SIZE ? c_compress_to_size(inputFullPath.toUtf8().constData(), outputFullPath.toUtf8().constData(), r_parameters, maxOutputSize, true) : c_compress(inputFullPath.toUtf8().constData(), outputFullPath.toUtf8().constData(), r_parameters);

    QFileInfo outputFileInfo(outputFullPath);
    CImage::setFileDates(outputFileInfo, compressionOptions.datesMap, inputFileDates);
    return result.success;
}

bool CImage::compress(const CompressionOptions& compressionOptions)
{
    // If caching originals is enabled, make sure a pristine copy of the original
    // exists BEFORE we (potentially) overwrite it. All source reads below use
    // this copy so re-compressing later still uses the uncompressed original.
    if (compressionOptions.cacheOriginals) {
        this->ensureCachedOriginal();
    }

    QString outputPath = compressionOptions.sameFolderAsInput ? this->getDirectory() : compressionOptions.outputPath;
    QString inputFullPath = this->getFullPath();
    // The path the image bytes are actually READ from (original, or its cached copy).
    QString sourceReadPath = this->getCompressionSourcePath(compressionOptions.cacheOriginals);
    QString suffix = compressionOptions.suffix;
    QFileInfo inputFileInfo = QFileInfo(inputFullPath);
    // Size/comparisons should reflect the actual source bytes being read.
    QFileInfo sourceFileInfo = QFileInfo(sourceReadPath);
    QString outputFormat = getOutputSupportedFormats()[compressionOptions.format];
    bool convert = compressionOptions.format != 0 && this->format.compare(outputFormat, Qt::CaseInsensitive) != 0;
    this->additionalInfo = "";
    if (!QFileInfo::exists(sourceReadPath)) {
        qCritical() << "File" << sourceReadPath << "does not exist.";
        this->additionalInfo = QIODevice::tr("Input file does not exist");
        return false;
    }
    QString outputSuffix = this->extension;
    if (compressionOptions.format != 0) {
        outputSuffix = getOutputSupportedFormats()[compressionOptions.format].toLower();
    }

    QString fullFileName = inputFileInfo.completeBaseName() + suffix + "." + outputSuffix;
    QString fullFileNameWithOriginalExtension = inputFileInfo.completeBaseName() + suffix + "." + this->extension;
    FileDates inputFileDates = {
        inputFileInfo.fileTime(QFile::FileBirthTime),
        inputFileInfo.fileTime(QFile::FileModificationTime),
        inputFileInfo.fileTime(QFile::FileAccessTime)
    };

    if (compressionOptions.keepStructure && !compressionOptions.sameFolderAsInput) {
        outputPath = inputFileInfo.absolutePath().remove(0, compressionOptions.basePath.length());
        outputPath = QDir::cleanPath(compressionOptions.outputPath + QDir::separator() + outputPath);
    }

    QDir outputDir(outputPath);
    if (!outputDir.exists()) {
        if (!outputDir.mkpath(outputPath)) {
            qCritical() << "Cannot make path" << outputPath;
            this->additionalInfo = QIODevice::tr("Cannot make output path, check your permissions");
            return false;
        }
    }

    QString tempFileFullPath = "";

    QString outputFullPath = outputDir.absoluteFilePath(fullFileName);
    bool outputAlreadyExists = QFile(outputFullPath).exists();

    QTemporaryFile tempFile(outputPath + QDir::separator() + inputFileInfo.completeBaseName() + ".XXXXXXXX." + outputSuffix);
    if (tempFile.open()) {
        tempFileFullPath = tempFile.fileName();
    }

    if (tempFileFullPath.isEmpty()) {
        qCritical() << "Temporary file" << tempFileFullPath << "is empty.";
        this->additionalInfo = QIODevice::tr("Temporary file creation failed");
        return false;
    }

    tempFile.close();

    QString inputCopyFile = sourceReadPath;
    QString compressionInput = sourceReadPath;
    if (convert) {
        QImage imageToBeConverted = QImage(sourceReadPath);
        imageToBeConverted = prepareImageForConversion(imageToBeConverted, compressionOptions.format, compressionOptions.transparencyFillColor);
        bool conversionSuccess = imageToBeConverted.save(tempFileFullPath, getOutputSupportedFormats().at(compressionOptions.format).toLower().toUtf8().constData(), 100);
        this->additionalInfo = QIODevice::tr("File conversion failed");
        if (!conversionSuccess) {
            return false;
        }
        compressionInput = tempFileFullPath;
    }

    QString previewPath = this->getTemporaryPreviewFullPath();
    if (QFile::exists(previewPath)) {
        if (QFile::remove(tempFileFullPath)) {
            QFile::copy(previewPath, tempFileFullPath);
        }
    }

    CCSParameters r_parameters = this->getCSParameters(compressionOptions);

    size_t maxOutputSize = getMaxOutputSizeInBytes(compressionOptions.maxOutputSize, sourceFileInfo.size());

    CCSResult result = compressionOptions.compressionMode == SIZE ? c_compress_to_size(compressionInput.toUtf8().constData(), tempFileFullPath.toUtf8().constData(), r_parameters, maxOutputSize, true) : c_compress(compressionInput.toUtf8().constData(), tempFileFullPath.toUtf8().constData(), r_parameters);

    if (result.success) {
        QFileInfo outputInfo(tempFileFullPath);

        bool outputIsBiggerThanInput = outputInfo.size() >= sourceFileInfo.size() && compressionOptions.skipIfBigger;
        bool inputAlreadyMoved = false;

        // If the output is bigger, and we are converting, we should fall back to the original file with original extension
        if (outputIsBiggerThanInput && convert) {
            outputFullPath = outputDir.absoluteFilePath(fullFileNameWithOriginalExtension);
            outputAlreadyExists = QFile(outputFullPath).exists();
        }

        if (outputAlreadyExists && !outputIsBiggerThanInput) {
            if (compressionOptions.sameFolderAsInput) {
                bool trashingResult = false;
                if (!compressionOptions.moveOriginalFile || (compressionOptions.moveOriginalFile && compressionOptions.moveOriginalFileDestination == 0)) {
                    trashingResult = QFile::moveToTrash(outputFullPath);
                }
                // Can fail in some conditions, like NAS storages. Fallback to normal remove.
                if (!trashingResult) {
                    QFile::remove(outputFullPath);
                }
                inputAlreadyMoved = true;
            } else {
                QFile::remove(outputFullPath);
            }
        }

        if (!outputIsBiggerThanInput) {
            inputCopyFile = tempFileFullPath;
        } else {
            this->status = CImageStatus::WARNING;
            this->additionalInfo = QIODevice::tr("Skipped: compressed file is bigger than original");
            if (outputAlreadyExists) {
                QFileInfo outputFileInfo = QFileInfo(outputFullPath);
                this->setCompressedInfo(outputFileInfo);
                return true;
            }
        }
        bool copyResult = QFile::copy(inputCopyFile, outputFullPath);
        if (!copyResult) {
            qCritical() << "Failed to copy from" << inputCopyFile << "to" << outputFullPath;
            this->additionalInfo = QIODevice::tr("Cannot copy output file, check your permissions");
            return false;
        }

        // If enabled AND the format was actually converted this run, remove the
        // source file this image was converted FROM (same base name + the source's
        // ORIGINAL extension) in the output directory. Only that one file is
        // touched — same-named files of other, unrelated formats are left alone.
        if (convert && compressionOptions.deleteOriginalFormatFile) {
            QString sourceExt = this->extension.toLower();
            // The source file keeps its original name (the output suffix only
            // applies to the converted output, not to the source we're removing).
            QString sourceFormatFile = outputDir.absoluteFilePath(inputFileInfo.completeBaseName() + "." + sourceExt);
            QFileInfo sourceInfo(sourceFormatFile);
            QFileInfo outputInfoForName(outputFullPath);
            // Never delete the file we just wrote.
            if (sourceInfo.absoluteFilePath() != outputInfoForName.absoluteFilePath() && sourceInfo.exists()) {
                if (!QFile::moveToTrash(sourceFormatFile)) {
                    QFile::remove(sourceFormatFile);
                }
            }
        }
        if (compressionOptions.moveOriginalFile && !inputAlreadyMoved) {
            if (compressionOptions.moveOriginalFileDestination == 0 && !QFile::moveToTrash(this->getFullPath())) {
                qWarning() << "Cannot move to trash file" << this->getFullPath();
                this->additionalInfo = QIODevice::tr("Cannot move original file to trash, check your permissions");
            } else if (compressionOptions.moveOriginalFileDestination == 1 && !QFile::remove(this->getFullPath())) {
                qWarning() << "Cannot delete file" << this->getFullPath();
                this->additionalInfo = QIODevice::tr("Cannot delete original file, check your permissions");
            }
        }

        // Cocos Creator support: when the image was actually converted to a new
        // format, the output was placed in the same folder as the original and
        // the original source file has been removed, the adjacent
        // "<name>.meta" file must follow the new file (rename + update the
        // format recorded inside it), otherwise the editor would reimport the
        // asset with a brand new uuid and break existing references.
        if (compressionOptions.syncMetaFile) {
            const QFileInfo convertedFileInfo(outputFullPath);
            if (convert
                && !outputIsBiggerThanInput
                && convertedFileInfo.completeBaseName() == inputFileInfo.completeBaseName()
                && QDir::cleanPath(convertedFileInfo.absolutePath()) == QDir::cleanPath(inputFileInfo.absolutePath())
                && !QFileInfo::exists(inputFileInfo.absoluteFilePath())) {
                CImage::syncMetaFileForConversion(inputFileInfo.absoluteFilePath(), convertedFileInfo.absoluteFilePath());
            }
        }

        QFileInfo outputFileInfo = QFileInfo(outputFullPath);
        if (compressionOptions.keepDates) {
            CImage::setFileDates(outputFileInfo, compressionOptions.datesMap, inputFileDates);
        }
        this->setCompressedInfo(outputFileInfo);
    } else {
        this->additionalInfo = result.error_message;
        qCritical() << "Compression result for i:" << inputFullPath << "and o: " << tempFileFullPath << "is false. Error:" << result.error_message;
    }

    return result.success;
}

CCSParameters CImage::getCSParameters(const CompressionOptions& compressionOptions) const
{
    CCSParameters r_parameters = {};
    r_parameters.keep_metadata = compressionOptions.keepMetadata;
    r_parameters.jpeg_quality = static_cast<unsigned int>(compressionOptions.jpegQuality);
    r_parameters.jpeg_chroma_subsampling = compressionOptions.jpegChromaSubsampling;
    r_parameters.jpeg_progressive = compressionOptions.jpegProgressive;
    r_parameters.png_quality = static_cast<unsigned int>(compressionOptions.pngQuality);
    r_parameters.png_optimization_level = static_cast<unsigned int>(compressionOptions.pngOptimizationLevel);
    r_parameters.png_force_zopfli = false;
    r_parameters.gif_quality = 20;
    r_parameters.webp_quality = static_cast<unsigned int>(compressionOptions.webpQuality);
    r_parameters.tiff_compression = static_cast<unsigned int>(compressionOptions.tiffMethod);
    r_parameters.tiff_deflate_level = static_cast<unsigned int>(compressionOptions.tiffDeflateLevel);
    r_parameters.optimize = compressionOptions.lossless;
    r_parameters.width = 0;
    r_parameters.height = 0;

    // Resize
    if (compressionOptions.resize) {
        QImageReader imageReader(this->getCompressionSourcePath(compressionOptions.cacheOriginals));
        QSize originalSize = imageReader.size();

        std::tuple<unsigned int, unsigned int> dimensions = cResize(&imageReader, compressionOptions);

        if (std::get<0>(dimensions) != originalSize.width() || std::get<1>(dimensions) != originalSize.height()) {
            r_parameters.width = static_cast<int>(std::get<0>(dimensions));
            r_parameters.height = static_cast<int>(std::get<1>(dimensions));
        }
    }
    return r_parameters;
}

void CImage::setCompressedInfo(const QFileInfo& fileInfo)
{
    QImageReader imageReader(fileInfo.canonicalFilePath());
    QSize imageSize = getSizeWithMetadata(&imageReader);
    this->compressedDirectory = fileInfo.canonicalPath();
    this->compressedSize = fileInfo.size();
    this->compressedFullPath = fileInfo.canonicalFilePath();
    this->compressedWidth = imageSize.width();
    this->compressedHeight = imageSize.height();
}

void CImage::setFileDates(const QFileInfo& fileInfo, FileDatesOutputOption datesMap, const FileDates& inputFileDates)
{
    QFile outputFile(fileInfo.canonicalFilePath());
    outputFile.open(QIODevice::ReadWrite);
    if (datesMap.keepCreation) {
        outputFile.setFileTime(inputFileDates.creation, QFileDevice::FileBirthTime);
    }
    if (datesMap.keepLastModified) {
        outputFile.setFileTime(inputFileDates.lastModified, QFileDevice::FileModificationTime);
    }
    if (datesMap.keepLastAccess) {
        outputFile.setFileTime(inputFileDates.lastAccess, QFileDevice::FileAccessTime);
    }
    outputFile.close();
}

QString CImage::getCompressedFullPath() const
{
    return compressedFullPath;
}

double CImage::getRatio() const
{
    return static_cast<double>(this->compressedSize) / static_cast<double>(this->size);
}

QString CImage::getFormattedSavedRatio() const
{
    return QString::number(round(100 - (this->getRatio() * 100))) + "%";
}

QString CImage::getRichFormattedSavedRatio() const
{
    return this->getFormattedSavedRatio();
}

CImageStatus CImage::getStatus() const
{
    return status;
}

void CImage::setStatus(const CImageStatus& value)
{
    status = value;
}

size_t CImage::getOriginalSize() const
{
    return this->size;
}

size_t CImage::getCompressedSize() const
{
    return this->compressedSize;
}

size_t CImage::getTotalPixels() const
{
    return this->width * this->height;
}

QString CImage::getFormattedStatus() const
{
    switch (this->status) {
    case CImageStatus::COMPRESSING:
        return QIODevice::tr("Compressing...");
    case CImageStatus::ERROR:
        return QIODevice::tr("Error:") + " " + this->additionalInfo;
    case CImageStatus::WARNING:
        return this->additionalInfo;
    case CImageStatus::COMPRESSED:
    case CImageStatus::UNCOMPRESSED:
    default:
        return "";
    }
}

QString CImage::getDirectory() const
{
    return this->directory;
}

QString CImage::getCompressedDirectory() const
{
    return this->compressedDirectory;
}

QString CImage::getHashedFullPath() const
{
    return this->hashedFullPath;
}

QString CImage::getTemporaryPreviewFullPath() const
{
    QString tempFileName = hashString(this->hashedFullPath + "." + getCompressionOptionsHash(), QCryptographicHash::Sha256);
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString temporaryPreviewFullPath = cacheDir + QDir::separator() + tempFileName;
    bool pathCreationSuccess = QDir(cacheDir).mkpath(cacheDir);
    if (!pathCreationSuccess) {
        return { "" };
    }
    return temporaryPreviewFullPath;
}

QString CImage::getPreviewFullPath() const
{
    if (!this->compressedFullPath.isEmpty()) {
        return this->compressedFullPath;
    }

    return this->getTemporaryPreviewFullPath();
}

QString CImage::getFormat() const
{
    return this->format;
}

size_t CImage::getMaxOutputSizeInBytes(MaxOutputSize maxOutputSize, size_t originalSize)
{
    if (maxOutputSize.unit == MAX_OUTPUT_PERCENTAGE) {
        return std::floor(originalSize * maxOutputSize.maxOutputSize / 100);
    }

    return maxOutputSize.maxOutputSize << (maxOutputSize.unit * 10);
}

QImage CImage::prepareImageForConversion(const QImage& source, int targetFormatIndex, const QString& fillColorHex)
{
    QImage image = source;
    if (image.isNull()) {
        return image;
    }

    // Normalize palette/mono images to a plain RGB(A) format so encoders can
    // handle them.
    if (image.format() == QImage::Format_Indexed8
        || image.format() == QImage::Format_Mono
        || image.format() == QImage::Format_MonoLSB) {
        image = image.hasAlphaChannel()
            ? image.convertToFormat(QImage::Format_ARGB32)
            : image.convertToFormat(QImage::Format_RGB32);
    }

    // JPG has no alpha channel: composite transparent pixels onto a solid fill
    // color so they don't turn into garbage.
    bool targetHasNoAlpha = (targetFormatIndex == 1); // 1 = JPG
    if (targetHasNoAlpha && image.hasAlphaChannel()) {
        QColor fill(fillColorHex.isEmpty() ? "#FFFFFF" : fillColorHex);
        if (!fill.isValid()) {
            fill = QColor(Qt::white);
        }
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.fill(fill);
        QPainter painter(&flattened);
        painter.drawImage(0, 0, image);
        painter.end();
        image = flattened;
    }

    return image;
}

QString CImage::getCachedOriginalPath() const
{
    // A stable, unique path for the cached copy of the ORIGINAL image, derived
    // from the original full path hash. Lives in an "originals" subfolder of the
    // app cache so it stays isolated from the preview cache.
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QDir::separator() + "originals";
    QString fileName = this->hashedFullPath + "." + this->extension;
    return cacheDir + QDir::separator() + fileName;
}

bool CImage::ensureCachedOriginal() const
{
    // Lazily copy the original image into the cache the first time we need it.
    // Subsequent (re-)compressions reuse the copy, so re-compressing after the
    // original was overwritten still uses the pristine source.
    QString cachedPath = this->getCachedOriginalPath();
    if (QFile::exists(cachedPath)) {
        return true;
    }

    QString cacheDir = QFileInfo(cachedPath).absolutePath();
    if (!QDir(cacheDir).mkpath(cacheDir)) {
        qWarning() << "Cannot create originals cache path" << cacheDir;
        return false;
    }

    if (!QFile::copy(this->fullPath, cachedPath)) {
        qWarning() << "Cannot cache original" << this->fullPath << "to" << cachedPath;
        return false;
    }
    return true;
}

QString CImage::getCompressionSourcePath(bool cacheOriginals) const
{
    if (cacheOriginals) {
        QString cachedPath = this->getCachedOriginalPath();
        if (QFile::exists(cachedPath)) {
            return cachedPath;
        }
    }
    return this->fullPath;
}

void CImage::removeCachedOriginal() const
{
    QString cachedPath = this->getCachedOriginalPath();
    if (QFile::exists(cachedPath)) {
        QFile::remove(cachedPath);
    }
}

bool CImage::restoreFromCache()
{
    QString cachedPath = this->getCachedOriginalPath();
    if (!QFile::exists(cachedPath)) {
        return false; // No cached original -> nothing to restore.
    }

    // 1) Copy the cached original back to its original location (name + format).
    if (QFile::exists(this->fullPath)) {
        QFile::remove(this->fullPath); // QFile::copy does not overwrite.
    }
    if (!QFile::copy(cachedPath, this->fullPath)) {
        qWarning() << "Restore failed: cannot copy" << cachedPath << "to" << this->fullPath;
        return false;
    }

    // 2) Remove the converted output produced last time, but ONLY if it differs
    //    from the restored original (i.e. a format conversion happened). Unrelated
    //    same-named files of other formats are never touched.
    if (!this->compressedFullPath.isEmpty()
        && QFileInfo(this->compressedFullPath).absoluteFilePath() != QFileInfo(this->fullPath).absoluteFilePath()
        && QFile::exists(this->compressedFullPath)) {
        if (!QFile::moveToTrash(this->compressedFullPath)) {
            QFile::remove(this->compressedFullPath);
        }
    }

    // 3) If the previous conversion moved the adjacent Cocos Creator .meta file
    //    to the converted output's name (e.g. 1.png.meta -> 1.jpg.meta), bring
    //    it back to the restored original's name so the asset keeps its uuid.
    if (!this->compressedFullPath.isEmpty()
        && QFileInfo(this->compressedFullPath).absoluteFilePath() != QFileInfo(this->fullPath).absoluteFilePath()) {
        const QString convertedMeta = this->compressedFullPath + QLatin1String(".meta");
        const QString originalMeta = this->fullPath + QLatin1String(".meta");
        if (QFile::exists(convertedMeta) && !QFile::exists(originalMeta)) {
            CImage::syncMetaFileForConversion(this->compressedFullPath, this->fullPath);
        }
    }

    // 4) Reset compression info back to the uncompressed original state.
    this->compressedFullPath.clear();
    this->compressedDirectory.clear();
    this->compressedSize = this->size;
    this->compressedWidth = this->width;
    this->compressedHeight = this->height;
    this->additionalInfo.clear();
    this->status = CImageStatus::UNCOMPRESSED;
    return true;
}

namespace {

// Recursively walk a parsed Cocos Creator meta document and collect the bare
// file-extension tokens (anything starting with "." that is not ".json", e.g.
// ".png"/".jpg") that appear inside any "files" array. These tokens describe
// the library files Cocos Creator produced for the asset.
void collectMetaFileExtensions(const QJsonValue& value, QStringList& extensions)
{
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key() == QLatin1String("files") && it.value().isArray()) {
                const QJsonArray arr = it.value().toArray();
                for (const QJsonValue& elem : arr) {
                    if (elem.isString()) {
                        const QString s = elem.toString();
                        if (s.startsWith(QLatin1Char('.'))
                            && s.compare(QLatin1String(".json"), Qt::CaseInsensitive) != 0
                            && !extensions.contains(s, Qt::CaseInsensitive)) {
                            extensions.append(s);
                        }
                    }
                }
            } else {
                collectMetaFileExtensions(it.value(), extensions);
            }
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue& elem : arr) {
            collectMetaFileExtensions(elem, extensions);
        }
    }
}

} // namespace

bool CImage::syncMetaFileForConversion(const QString& sourceImagePath, const QString& outputImagePath)
{
    const QFileInfo sourceInfo(sourceImagePath);
    const QFileInfo outputInfo(outputImagePath);
    const QString sourceExt = sourceInfo.suffix();
    const QString outputExt = outputInfo.suffix();
    if (sourceExt.compare(outputExt, Qt::CaseInsensitive) == 0) {
        return false;
    }

    const QString metaPath = sourceImagePath + QLatin1String(".meta");
    const QString newMetaPath = outputImagePath + QLatin1String(".meta");
    if (!QFile::exists(metaPath)) {
        return false;
    }
    if (QFileInfo(metaPath).absoluteFilePath() == QFileInfo(newMetaPath).absoluteFilePath()) {
        return false;
    }

    QFile metaFile(metaPath);
    if (!metaFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read Cocos Creator meta file:" << metaPath;
        return false;
    }
    QString content = QString::fromUtf8(metaFile.readAll());
    metaFile.close();

    // Cocos Creator records the library file extensions it produced as quoted
    // tokens in the "files" arrays (e.g. ".json", ".png"). We swap them
    // textually so the rest of the file bytes and formatting are preserved.
    const QString newToken = QStringLiteral("\".%1\"").arg(outputExt.toLower());
    bool contentChanged = false;

    // Compatibility rule: only modify the file contents when it really is JSON
    // AND actually contains "files" arrays with an extension entry we can swap.
    // In every other situation - not JSON, JSON without a "files" array, a
    // "files" array that only lists ".json", ... - the contents are left
    // byte-for-byte untouched and only the file name is changed below.
    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
        const QJsonValue root = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
        QStringList extensions;
        collectMetaFileExtensions(root, extensions);
        for (const QString& ext : extensions) {
            // The tokens appear quoted in the raw text, e.g. ".jpg"; replace
            // them with the new one, preserving the original file layout.
            const QString quotedExt = QLatin1Char('"') + ext + QLatin1Char('"');
            if (quotedExt.compare(newToken, Qt::CaseInsensitive) == 0) {
                continue;
            }
            content.replace(quotedExt, newToken, Qt::CaseInsensitive);
            contentChanged = true;
        }
    }

    // Write the updated content back in place first, then move the .meta next
    // to the converted image so the rename cannot leave a stale file behind.
    if (contentChanged) {
        if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "Cannot write Cocos Creator meta file:" << metaPath;
            return false;
        }
        metaFile.write(content.toUtf8());
        metaFile.close();
    }

    // Remove a pre-existing target so the rename cannot fail on it.
    if (QFile::exists(newMetaPath) && !QFile::remove(newMetaPath)) {
        qWarning() << "Cannot remove existing meta file:" << newMetaPath;
        return false;
    }
    if (!QFile::rename(metaPath, newMetaPath)) {
        qWarning() << "Cannot rename Cocos Creator meta file:" << metaPath << "to" << newMetaPath;
        return false;
    }
    return true;
}
