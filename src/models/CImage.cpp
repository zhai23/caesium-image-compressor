#include "CImage.h"
#include "../include/libcaesium.h"
#include "utils/ImageIO.h"
#include "utils/TgaCodec.h"

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
#include <QMap>
#include <QPainter>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <cmath>

CImage::CImage(const QString& path)
{
    QFileInfo fileInfo = QFileInfo(path);
    this->format = ImageIO::format(path);

    if (!supportedFormats.contains(format)) {
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

    QSize imageSize = ImageIO::size(path);
    if (!imageSize.isValid() || imageSize.isEmpty()) throw ImageNotSupportedException();
    this->width = imageSize.width();
    this->height = imageSize.height();

    this->compressedWidth = this->width;
    this->compressedHeight = this->height;

    this->hashedFullPath = hashString(this->fullPath, QCryptographicHash::Sha256);

    this->status = CImageStatus::UNCOMPRESSED;

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
    if (outputFormat == "TGA" || (compressionOptions.format == 0 && this->format == "tga")) {
        QString error;
        return writeTgaOutput(inputFullPath, outputFullPath, compressionOptions, error);
    }
    bool convert = compressionOptions.format != 0 && this->getFormat().compare(outputFormat, Qt::CaseInsensitive) != 0;
    FileDates inputFileDates = {
        inputFileInfo.fileTime(QFile::FileBirthTime),
        inputFileInfo.fileTime(QFile::FileModificationTime),
        inputFileInfo.fileTime(QFile::FileAccessTime)
    };
    CCSParameters r_parameters = this->getCSParameters(compressionOptions);
    if (convert) {
        QImage imageToBeConverted = ImageIO::read(inputFullPath, false);
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
        if (!this->ensureCachedOriginal()) {
            this->additionalInfo = QIODevice::tr("Cannot cache original image");
            return false;
        }
    }

    QString outputPath = compressionOptions.sameFolderAsInput ? this->getDirectory() : compressionOptions.outputPath;
    QString inputFullPath = this->getFullPath();
    // The path the image bytes are actually READ from (original, or its cached copy).
    QString sourceReadPath = this->getCompressionSourcePath(compressionOptions.cacheOriginals);
    QString suffix = compressionOptions.suffix;
    QFileInfo inputFileInfo = QFileInfo(inputFullPath);
    // Size/comparisons should reflect the actual source bytes being read.
    QFileInfo sourceFileInfo = QFileInfo(sourceReadPath);
    // The image as it currently sits on disk: bitmap-font atlas descriptors
    // describe THIS file, so their metrics have to be re-scaled from its size
    // (a re-compression may start from a previously resized image).
    QSize imageDimsBeforeCompression = ImageIO::size(inputFullPath);
    if (!imageDimsBeforeCompression.isValid() || imageDimsBeforeCompression.isEmpty()) {
        imageDimsBeforeCompression = QSize(this->width, this->height);
    }
    QString outputFormat = getOutputSupportedFormats()[compressionOptions.format];
    const bool tgaOutput = outputFormat == "TGA" || (compressionOptions.format == 0 && this->format == "tga");
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
    if (convert && !tgaOutput) {
        QImage imageToBeConverted = ImageIO::read(sourceReadPath, false);
        imageToBeConverted = prepareImageForConversion(imageToBeConverted, compressionOptions.format, compressionOptions.transparencyFillColor);
        bool conversionSuccess = imageToBeConverted.save(tempFileFullPath, getOutputSupportedFormats().at(compressionOptions.format).toLower().toUtf8().constData(), 100);
        this->additionalInfo = QIODevice::tr("File conversion failed");
        if (!conversionSuccess) {
            return false;
        }
        compressionInput = tempFileFullPath;
    }

    QString previewPath = this->getTemporaryPreviewFullPath();
    if (!tgaOutput && QFile::exists(previewPath)) {
        if (QFile::remove(tempFileFullPath)) {
            QFile::copy(previewPath, tempFileFullPath);
        }
    }

    CCSParameters r_parameters = this->getCSParameters(compressionOptions);

    size_t maxOutputSize = getMaxOutputSizeInBytes(compressionOptions.maxOutputSize, sourceFileInfo.size());

    QString tgaError;
    QByteArray tgaErrorBytes;
    CCSResult result {};
    if (tgaOutput) {
        result.success = writeTgaOutput(sourceReadPath, tempFileFullPath, compressionOptions, tgaError);
        tgaErrorBytes = tgaError.toUtf8();
        result.error_message = tgaErrorBytes.constData();
    } else {
        result = compressionOptions.compressionMode == SIZE ? c_compress_to_size(compressionInput.toUtf8().constData(), tempFileFullPath.toUtf8().constData(), r_parameters, maxOutputSize, true) : c_compress(compressionInput.toUtf8().constData(), tempFileFullPath.toUtf8().constData(), r_parameters);
    }

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
        if (convert && !outputIsBiggerThanInput && compressionOptions.deleteOriginalFormatFile) {
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

        // Game engine support: when the image was actually converted to a new
        // format, the output was placed in the same folder as the original and
        // the original source file has been removed, the adjacent sidecar
        // files must follow the new file, otherwise they would keep pointing
        // to the removed image.
        // - Cocos Creator "<name>.meta": rename it (1.png.meta -> 1.jpg.meta)
        //   and update the format entry recorded inside it (the "files" list),
        //   otherwise the editor would reimport the asset with a brand new
        //   uuid and break existing references.
        // - Spine "<name>.atlas" / "<name>.atlas.txt": update the image file
        //   name recorded in its first line ("1.png" -> "1.jpg").
        const QFileInfo convertedFileInfo(outputFullPath);
        const bool sameTargetName = convertedFileInfo.completeBaseName() == inputFileInfo.completeBaseName();
        const bool sameTargetFolder = QDir::cleanPath(convertedFileInfo.absolutePath()) == QDir::cleanPath(inputFileInfo.absolutePath());
        const bool convertedInPlace = convert
            && !outputIsBiggerThanInput
            && sameTargetName
            && sameTargetFolder
            && !QFileInfo::exists(inputFileInfo.absoluteFilePath());
        if (convertedInPlace && compressionOptions.syncMetaFile) {
            CImage::syncMetaFileForConversion(inputFileInfo.absoluteFilePath(), convertedFileInfo.absoluteFilePath());
        }
        if (convertedInPlace && compressionOptions.syncSpineFile) {
            CImage::syncSpineFileForConversion(inputFileInfo.absoluteFilePath(), convertedFileInfo.absoluteFilePath());
        }

        QFileInfo outputFileInfo = QFileInfo(outputFullPath);
        if (compressionOptions.keepDates) {
            CImage::setFileDates(outputFileInfo, compressionOptions.datesMap, inputFileDates);
        }
        this->setCompressedInfo(outputFileInfo);

        // Bitmap-font atlas support: the .fnt / .fnt.meta / *.labelatlas.meta
        // descriptors of an atlas font record the image file name and the
        // pixel metrics of every glyph, so a conversion (recorded name) and a
        // resize (all metrics) both have to be written back. Contrary to the
        // sidecars above, this also covers a pure resize overwriting the image
        // under its own name.
        const bool imageReplacedInPlace = convertedInPlace
            || QDir::cleanPath(outputFullPath) == QDir::cleanPath(inputFileInfo.absoluteFilePath());
        if (compressionOptions.syncFontAtlasFiles && !outputIsBiggerThanInput && imageReplacedInPlace) {
            CImage::syncFontAtlasFiles(
                inputFileInfo.absoluteFilePath(), imageDimsBeforeCompression,
                convertedFileInfo.absoluteFilePath(), QSize(this->compressedWidth, this->compressedHeight));
        }
    } else {
        this->additionalInfo = QString::fromUtf8(result.error_message);
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
        QSize originalSize = this->format == "tga" ? ImageIO::size(imageReader.fileName()) : imageReader.size();

        std::tuple<unsigned int, unsigned int> dimensions = this->format == "tga"
            ? cResize(originalSize, compressionOptions) : cResize(&imageReader, compressionOptions);

        if (std::get<0>(dimensions) != originalSize.width() || std::get<1>(dimensions) != originalSize.height()) {
            r_parameters.width = static_cast<int>(std::get<0>(dimensions));
            r_parameters.height = static_cast<int>(std::get<1>(dimensions));
        }
    }
    return r_parameters;
}

bool CImage::writeTgaOutput(const QString& sourcePath, const QString& outputPath,
    const CompressionOptions& options, QString& error) const
{
    if (options.keepMetadata && this->format == "tga" && TgaCodec::hasMetadata(sourcePath)) {
        error = QIODevice::tr("TGA metadata cannot be preserved. Disable Keep metadata to continue.");
        return false;
    }
    QImage image = ImageIO::read(sourcePath);
    if (image.isNull()) {
        error = QIODevice::tr("Cannot decode source image");
        return false;
    }
    if (options.resize) {
        // ImageIO has already applied orientation; resize in displayed coordinates.
        const auto dimensions = cResize(image.size(), options);
        const int width = int(std::get<0>(dimensions));
        const int height = int(std::get<1>(dimensions));
        if (width && height) image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        else if (width) image = image.scaledToWidth(width, Qt::SmoothTransformation);
        else if (height) image = image.scaledToHeight(height, Qt::SmoothTransformation);
    }
    // compress() supplies an existing QTemporaryFile. On Windows its handle
    // prevents QSaveFile from renaming over it, so write this staging file
    // directly; publication/overwrite happens only after successful encoding.
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = output.errorString();
        return false;
    }
    const bool written = TgaCodec::write(output, image, &error, options.tgaRle);
    const bool flushed = output.flush();
    output.close();
    if (!written || !flushed) {
        if (error.isEmpty()) error = output.errorString();
        QFile::remove(outputPath);
        return false;
    }
    if (options.compressionMode == SIZE
        && quint64(QFileInfo(outputPath).size()) > getMaxOutputSizeInBytes(options.maxOutputSize, QFileInfo(sourcePath).size())) {
        QFile::remove(outputPath);
        error = QIODevice::tr("TGA output cannot meet the requested size. Choose another format or resize the image.");
        return false;
    }
    return true;
}

void CImage::setCompressedInfo(const QFileInfo& fileInfo)
{
    QSize imageSize = ImageIO::size(fileInfo.canonicalFilePath());
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

bool CImage::restoreFromCache(bool syncFontAtlas)
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

    // 4) If the previous conversion rewrote the image name recorded in the
    //    adjacent Spine atlas file (e.g. "1.png" -> "1.jpg" in 1.atlas), bring
    //    the recorded name back to the restored original. The atlas file name
    //    itself never changes, only its first line, and it is left untouched
    //    when it does not point to the converted output.
    if (!this->compressedFullPath.isEmpty()
        && QFileInfo(this->compressedFullPath).absoluteFilePath() != QFileInfo(this->fullPath).absoluteFilePath()) {
        CImage::syncSpineFileForConversion(this->compressedFullPath, this->fullPath);
    }

    // 5) If the previous compression rewrote the bitmap-font atlas
    //    descriptors of the image (.fnt / .fnt.meta / *.labelatlas.meta),
    //    bring their recorded image name and glyph metrics back to the
    //    restored original. This is opt-in (the caller passes the current
    //    state of the sync option) because the metrics can only be scaled
    //    back when they were scaled on compression in the first place.
    if (syncFontAtlas && !this->compressedFullPath.isEmpty()) {
        CImage::syncFontAtlasFiles(this->compressedFullPath,
            QSize(this->compressedWidth, this->compressedHeight),
            this->fullPath, QSize(this->width, this->height));
    }

    // 6) Reset compression info back to the uncompressed original state.
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

// ---------------------------------------------------------------------------
// Bitmap-font atlas descriptors: AngelCode text .fnt files and the Cocos
// Creator metas of bitmap fonts (".fnt.meta") and art fonts
// (".labelatlas.meta"). All of them record the atlas image name and pixel
// metrics, so a format conversion (the recorded name) and a resize (every
// metric) must be written back. They are edited textually so the
// Cocos-written formatting, key order and number styles are preserved: a
// JSON round-trip would sort the keys and re-indent the whole file.

// A number as written in a descriptor file.
const QString ATLAS_NUMBER = QStringLiteral("-?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?");

// Scale one recorded number while keeping its text style: integral values
// keep a plain integer ("69" -> "35"), fractional ones keep their decimals
// ("60.72" -> "30.36"). integralValues is set for the .fnt interchange format
// and the metas derived from it, which only hold integers; art-font metas are
// the data itself and keep full precision so scaling is lossless.
QString scaledAtlasNumber(const QString& valueText, double scale, bool integralValues)
{
    bool ok = false;
    const double value = valueText.toDouble(&ok);
    if (!ok) {
        return valueText;
    }
    const bool integral = integralValues
        && !valueText.contains(QLatin1Char('.'))
        && !valueText.contains(QLatin1Char('e'), Qt::CaseInsensitive);
    if (integral) {
        return QString::number(qRound64(value * scale));
    }
    double scaled = std::round(value * scale * 1000000.0) / 1000000.0;
    // Snap to the coarsest decimal grid the value still matches, so scaling a
    // metric away and back does not leave crumbs like "69.000001" behind: the
    // error of one scaling step is absorbed by the next one.
    for (int decimals = 0; decimals < 6; ++decimals) {
        const double factor = std::pow(10.0, decimals);
        const double snapped = std::round(scaled * factor) / factor;
        if (std::abs(scaled - snapped) < 0.00001) {
            scaled = snapped;
            break;
        }
    }
    QString text = QString::number(scaled, 'f', 6);
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

// Rewrite every number recorded for one of the given keys ("<key>": <number>
// in a JSON meta). Each key brings its own scale factor because x metrics and
// y metrics scale differently. Returns how many values were changed.
int scaleAtlasJsonNumbers(QString& content, const QMap<QString, double>& keyScales, bool integralValues)
{
    int changed = 0;
    for (auto it = keyScales.constBegin(); it != keyScales.constEnd(); ++it) {
        const QRegularExpression re(QStringLiteral("\"%1\"\\s*:\\s*(%2)")
            .arg(QRegularExpression::escape(it.key()), ATLAS_NUMBER));
        QList<QRegularExpressionMatch> matches;
        QRegularExpressionMatchIterator matchIt = re.globalMatch(content);
        while (matchIt.hasNext()) {
            matches.append(matchIt.next());
        }
        for (int i = matches.size() - 1; i >= 0; --i) {
            const QRegularExpressionMatch& match = matches.at(i);
            const QString valueText = match.captured(1);
            const QString scaled = scaledAtlasNumber(valueText, it.value(), integralValues);
            if (scaled == valueText) {
                continue;
            }
            content.replace(match.capturedStart(1), match.capturedEnd(1) - match.capturedStart(1), scaled);
            ++changed;
        }
    }
    return changed;
}

// Same for the key=<number> tokens of an AngelCode .fnt line. The key has to
// start a token so "x=" is never taken from "xoffset=". The .fnt format is
// integer based, so values are always rounded.
int scaleAtlasFntNumbers(QString& line, const QMap<QString, double>& keyScales)
{
    int changed = 0;
    for (auto it = keyScales.constBegin(); it != keyScales.constEnd(); ++it) {
        const QRegularExpression re(QStringLiteral("(?:^|\\s)(%1)=(%2)")
            .arg(QRegularExpression::escape(it.key()), ATLAS_NUMBER));
        QList<QRegularExpressionMatch> matches;
        QRegularExpressionMatchIterator matchIt = re.globalMatch(line);
        while (matchIt.hasNext()) {
            matches.append(matchIt.next());
        }
        for (int i = matches.size() - 1; i >= 0; --i) {
            const QRegularExpressionMatch& match = matches.at(i);
            const QString valueText = match.captured(2);
            const QString scaled = scaledAtlasNumber(valueText, it.value(), true);
            if (scaled == valueText) {
                continue;
            }
            line.replace(match.capturedStart(2), match.capturedEnd(2) - match.capturedStart(2), scaled);
            ++changed;
        }
    }
    return changed;
}

// Set one key=<value> token of a .fnt line to a fixed value. Returns 1 when
// the line changed.
int setAtlasFntValue(QString& line, const QString& key, const QString& valueText)
{
    const QRegularExpression re(QStringLiteral("(?:^|\\s)(%1)=(%2)")
        .arg(QRegularExpression::escape(key), ATLAS_NUMBER));
    const QRegularExpressionMatch match = re.match(line);
    if (!match.hasMatch() || match.captured(2) == valueText) {
        return 0;
    }
    line.replace(match.capturedStart(2), match.capturedEnd(2) - match.capturedStart(2), valueText);
    return 1;
}

// Scale every number inside the "kerningDict": { ... } object of a meta: they
// are the kerning amounts, which are x-axis metrics. Its keys ("43_45") live
// inside strings and are never taken for values.
int scaleAtlasKerningNumbers(QString& content, double scale, bool integralValues)
{
    static const QRegularExpression marker(QStringLiteral("\"kerningDict\"\\s*:\\s*\\{"));
    const QRegularExpressionMatch markerMatch = marker.match(content);
    if (!markerMatch.hasMatch()) {
        return 0;
    }
    const int objectStart = content.indexOf(QLatin1Char('{'), markerMatch.capturedStart());
    if (objectStart < 0) {
        return 0;
    }

    QList<QPair<int, int>> spans;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (int i = objectStart; i < content.size(); ++i) {
        const QChar c = content.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\')) {
                escaped = true;
            } else if (c == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (c == QLatin1Char('{') || c == QLatin1Char('[')) {
            ++depth;
            continue;
        }
        if (c == QLatin1Char('}') || c == QLatin1Char(']')) {
            --depth;
            if (depth == 0) {
                break;
            }
            continue;
        }
        if (!c.isDigit() && c != QLatin1Char('-') && c != QLatin1Char('.')) {
            continue;
        }
        int end = i;
        while (end < content.size()) {
            const QChar d = content.at(end);
            if (!d.isDigit() && d != QLatin1Char('-') && d != QLatin1Char('+')
                && d != QLatin1Char('.') && d != QLatin1Char('e') && d != QLatin1Char('E')) {
                break;
            }
            ++end;
        }
        bool ok = false;
        content.mid(i, end - i).toDouble(&ok);
        if (ok) {
            spans.append(qMakePair(i, end));
        }
        i = end - 1;
    }

    int changed = 0;
    for (int i = spans.size() - 1; i >= 0; --i) {
        const int start = spans.at(i).first;
        const int length = spans.at(i).second - start;
        const QString valueText = content.mid(start, length);
        const QString scaled = scaledAtlasNumber(valueText, scale, integralValues);
        if (scaled == valueText) {
            continue;
        }
        content.replace(start, length, scaled);
        ++changed;
    }
    return changed;
}

// Point every image name matched by valueRegex (its first capture group is the
// name) at the converted file. Only the file extension changes
// ("texture.tga" -> "texture.png"); extension-less names ("texture", as kept
// by art-font metas) and names of other images are left untouched. Returns how
// many names were changed.
int renameAtlasImageNames(QString& content, const QRegularExpression& valueRegex,
    const QString& sourceFileName, const QString& outputFileName)
{
    QList<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator matchIt = valueRegex.globalMatch(content);
    while (matchIt.hasNext()) {
        matches.append(matchIt.next());
    }
    int changed = 0;
    for (int i = matches.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch& match = matches.at(i);
        const QString value = match.captured(1);
        const int slash = qMax(value.lastIndexOf(QLatin1Char('/')), value.lastIndexOf(QLatin1Char('\\')));
        if (value.mid(slash + 1).compare(sourceFileName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString replacement = value.left(slash + 1) + outputFileName;
        if (replacement == value) {
            continue;
        }
        content.replace(match.capturedStart(1), match.capturedEnd(1) - match.capturedStart(1), replacement);
        ++changed;
    }
    return changed;
}

// A Cocos Creator meta names its atlas image in "atlasName" ("texture.tga"
// for bitmap fonts, the bare "texture" for art fonts). Metas without such a
// name fall back to their own file name ("texture.fnt.meta" belongs to
// texture.<ext>).
bool atlasMetaReferencesImage(const QString& content, const QString& imageFileName,
    const QString& imageBaseName, const QString& metaBaseName)
{
    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject userData = doc.object().value(QLatin1String("userData")).toObject();
    QStringList recordedNames;
    for (const QJsonObject& holder : { userData, userData.value(QLatin1String("_fntConfig")).toObject() }) {
        const QJsonValue name = holder.value(QLatin1String("atlasName"));
        if (name.isString() && !name.toString().isEmpty()) {
            recordedNames.append(name.toString());
        }
    }
    if (recordedNames.isEmpty()) {
        return metaBaseName.compare(imageBaseName, Qt::CaseInsensitive) == 0;
    }
    for (const QString& recorded : recordedNames) {
        const int slash = qMax(recorded.lastIndexOf(QLatin1Char('/')), recorded.lastIndexOf(QLatin1Char('\\')));
        const QString name = recorded.mid(slash + 1);
        if (name.compare(imageFileName, Qt::CaseInsensitive) == 0
            || name.compare(imageBaseName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
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

bool CImage::syncSpineFileForConversion(const QString& sourceImagePath, const QString& outputImagePath)
{
    const QFileInfo sourceInfo(sourceImagePath);
    const QFileInfo outputInfo(outputImagePath);
    const QString sourceExt = sourceInfo.suffix();
    const QString outputExt = outputInfo.suffix();
    if (sourceExt.compare(outputExt, Qt::CaseInsensitive) == 0) {
        return false;
    }
    if (sourceInfo.fileName().isEmpty() || outputInfo.fileName().isEmpty()) {
        return false;
    }

    // Spine atlases are named after the image base name ("hdjl.atlas" or
    // "hdjl.atlas.txt" for "hdjl.png"). A format conversion never changes the
    // image base name, so the atlas file itself is not renamed: only the image
    // file name recorded in its first line has to follow the new extension.
    const QDir sourceDir = sourceInfo.absoluteDir();
    const QString baseName = sourceInfo.completeBaseName();
    const QStringList atlasPaths = {
        sourceDir.filePath(baseName + QLatin1String(".atlas")),
        sourceDir.filePath(baseName + QLatin1String(".atlas.txt")),
    };

    bool updated = false;
    for (const QString& atlasPath : atlasPaths) {
        QFile atlasFile(atlasPath);
        if (!atlasFile.exists()) {
            continue;
        }
        if (!atlasFile.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read Spine atlas file:" << atlasPath;
            continue;
        }
        QString content = QString::fromUtf8(atlasFile.readAll());
        atlasFile.close();

        // The first line of the atlas is the image file name; everything after
        // the line break is left byte-for-byte untouched.
        const int lineBreak = content.indexOf(QLatin1Char('\n'));
        const QString firstLine = lineBreak >= 0 ? content.left(lineBreak) : content;
        const QString remainder = lineBreak >= 0 ? content.mid(lineBreak) : QString();

        // The recorded name is surrounded by the line break and possibly some
        // whitespace, and it may carry a directory prefix to keep.
        int nameEnd = firstLine.size();
        while (nameEnd > 0 && firstLine.at(nameEnd - 1).isSpace()) {
            --nameEnd;
        }
        int nameStart = 0;
        while (nameStart < nameEnd && firstLine.at(nameStart).isSpace()) {
            ++nameStart;
        }
        const QString core = firstLine.mid(nameStart, nameEnd - nameStart);
        const int slash = qMax(core.lastIndexOf(QLatin1Char('/')), core.lastIndexOf(QLatin1Char('\\')));
        const QString prefix = core.left(slash + 1);
        const QString recordedName = core.mid(slash + 1);

        QString replacement = outputInfo.fileName();
        if (recordedName.compare(sourceInfo.fileName(), Qt::CaseInsensitive) != 0) {
            // Some exports glue the page attributes right after the image name
            // ("hdjl.pngsize:1102,3976"): in that case only the leading file
            // name is swapped and the rest of the line is kept as it is.
            if (!recordedName.startsWith(sourceInfo.fileName(), Qt::CaseInsensitive)) {
                continue;
            }
            replacement = outputInfo.fileName() + recordedName.mid(sourceInfo.fileName().size());
        }

        const QString updatedContent = firstLine.left(nameStart) + prefix + replacement
            + firstLine.mid(nameEnd) + remainder;
        if (!atlasFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "Cannot write Spine atlas file:" << atlasPath;
            continue;
        }
        atlasFile.write(updatedContent.toUtf8());
        atlasFile.close();
        updated = true;
    }
    return updated;
}

bool CImage::syncFontAtlasFiles(const QString& sourceImagePath, const QSize& sourceSize,
    const QString& outputImagePath, const QSize& outputSize)
{
    static const QRegularExpression pageFileRegex(QStringLiteral("file=\"([^\"]*)\""));
    static const QRegularExpression atlasNameRegex(QStringLiteral("\"atlasName\"\\s*:\\s*\"([^\"]*)\""));

    const QFileInfo sourceInfo(sourceImagePath);
    const QFileInfo outputInfo(outputImagePath);
    const QString sourceFileName = sourceInfo.fileName();
    const QString outputFileName = outputInfo.fileName();
    if (sourceFileName.isEmpty() || outputFileName.isEmpty()) {
        return false;
    }
    if (!sourceSize.isValid() || !outputSize.isValid()
        || sourceSize.width() <= 0 || sourceSize.height() <= 0
        || outputSize.width() <= 0 || outputSize.height() <= 0) {
        return false;
    }
    const bool nameChanged = sourceFileName.compare(outputFileName, Qt::CaseInsensitive) != 0;
    const double scaleX = static_cast<double>(outputSize.width()) / sourceSize.width();
    const double scaleY = static_cast<double>(outputSize.height()) / sourceSize.height();
    const bool scaleChanged = scaleX != 1.0 || scaleY != 1.0;
    if (!nameChanged && !scaleChanged) {
        return false;
    }

    const QString sourceBaseName = sourceInfo.completeBaseName();
    const QDir directory = sourceInfo.absoluteDir();
    // Every glyph metric is paired with the axis it belongs to.
    const QMap<QString, double> jsonKeys = {
        { QStringLiteral("fontSize"), scaleY },
        { QStringLiteral("commonHeight"), scaleY },
        { QStringLiteral("itemHeight"), scaleY },
        { QStringLiteral("y"), scaleY },
        { QStringLiteral("height"), scaleY },
        { QStringLiteral("yOffset"), scaleY },
        { QStringLiteral("itemWidth"), scaleX },
        { QStringLiteral("x"), scaleX },
        { QStringLiteral("width"), scaleX },
        { QStringLiteral("xOffset"), scaleX },
        { QStringLiteral("xAdvance"), scaleX },
    };

    bool updated = false;

    // 1) AngelCode text .fnt atlases.
    const QStringList fntFiles = directory.entryList({ QLatin1String("*.fnt") }, QDir::Files);
    for (const QString& fntFile : fntFiles) {
        QFile file(directory.filePath(fntFile));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read bitmap font file:" << file.fileName();
            continue;
        }
        const QByteArray raw = file.readAll();
        file.close();
        if (raw.left(3) == QByteArrayLiteral("BMF")) {
            continue; // Binary BMF atlases are not edited.
        }
        QString content = QString::fromUtf8(raw);
        QStringList lines = content.split(QLatin1Char('\n'));

        // The page line names the atlas image. Only single-page atlases are
        // edited: the layout of a multi-page atlas is bound to every page
        // image, and Cocos Creator bitmap fonts are single page.
        QList<int> pageLines;
        QString recordedName;
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines.at(i).startsWith(QLatin1String("page "))) {
                continue;
            }
            pageLines.append(i);
            const QRegularExpressionMatch match = pageFileRegex.match(lines.at(i));
            if (match.hasMatch()) {
                recordedName = match.captured(1);
            }
        }
        if (pageLines.size() != 1) {
            continue;
        }
        const int nameSlash = qMax(recordedName.lastIndexOf(QLatin1Char('/')), recordedName.lastIndexOf(QLatin1Char('\\')));
        const QString recordedFile = recordedName.mid(nameSlash + 1);
        if (recordedFile.compare(sourceFileName, Qt::CaseInsensitive) != 0
            && recordedFile.compare(sourceBaseName, Qt::CaseInsensitive) != 0) {
            continue;
        }

        int changed = 0;
        for (QString& line : lines) {
            const int space = line.indexOf(QLatin1Char(' '));
            const QString kind = space < 0 ? line : line.left(space);
            if (kind == QLatin1String("page")) {
                changed += renameAtlasImageNames(line, pageFileRegex, sourceFileName, outputFileName);
                continue;
            }
            if (!scaleChanged) {
                continue;
            }
            if (kind == QLatin1String("info")) {
                changed += scaleAtlasFntNumbers(line, { { QStringLiteral("size"), scaleY } });
            } else if (kind == QLatin1String("common")) {
                changed += scaleAtlasFntNumbers(line, {
                    { QStringLiteral("lineHeight"), scaleY },
                    { QStringLiteral("base"), scaleY },
                });
                changed += setAtlasFntValue(line, QStringLiteral("scaleW"), QString::number(outputSize.width()));
                changed += setAtlasFntValue(line, QStringLiteral("scaleH"), QString::number(outputSize.height()));
            } else if (kind == QLatin1String("char")) {
                changed += scaleAtlasFntNumbers(line, {
                    { QStringLiteral("x"), scaleX },
                    { QStringLiteral("y"), scaleY },
                    { QStringLiteral("width"), scaleX },
                    { QStringLiteral("height"), scaleY },
                    { QStringLiteral("xoffset"), scaleX },
                    { QStringLiteral("yoffset"), scaleY },
                    { QStringLiteral("xadvance"), scaleX },
                });
            } else if (kind == QLatin1String("kerning")) {
                changed += scaleAtlasFntNumbers(line, { { QStringLiteral("amount"), scaleX } });
            }
        }
        const QString updatedContent = lines.join(QLatin1Char('\n'));
        if (changed == 0 || updatedContent == content) {
            continue;
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "Cannot write bitmap font file:" << file.fileName();
            continue;
        }
        file.write(updatedContent.toUtf8());
        file.close();
        updated = true;
    }

    // 2) Cocos Creator metas of bitmap fonts (".fnt.meta") and art fonts
    //    (".labelatlas.meta").
    const QStringList metaFiles = directory.entryList({ QLatin1String("*.fnt.meta"), QLatin1String("*.labelatlas.meta") }, QDir::Files);
    for (const QString& metaFile : metaFiles) {
        QFile file(directory.filePath(metaFile));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read bitmap font meta file:" << file.fileName();
            continue;
        }
        QString content = QString::fromUtf8(file.readAll());
        file.close();

        // Metas without a recorded name are matched by their own file name:
        // "texture.fnt.meta" belongs to texture.<ext>.
        const QString metaBaseName = QFileInfo(QFileInfo(metaFile).completeBaseName()).completeBaseName();
        if (!atlasMetaReferencesImage(content, sourceFileName, sourceBaseName, metaBaseName)) {
            continue;
        }

        // The .fnt interchange format and the meta derived from it only hold
        // integer metrics; an art-font meta is the data itself (its fontSize is
        // already fractional) and keeps full precision, which also makes the
        // scaling lossless in both directions.
        const bool integralValues = !metaFile.endsWith(QLatin1String(".labelatlas.meta"));
        QString updatedContent = content;
        int changed = renameAtlasImageNames(updatedContent, atlasNameRegex, sourceFileName, outputFileName);
        if (scaleChanged) {
            changed += scaleAtlasJsonNumbers(updatedContent, jsonKeys, integralValues);
            changed += scaleAtlasKerningNumbers(updatedContent, scaleX, integralValues);
        }
        if (changed == 0 || updatedContent == content) {
            continue;
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "Cannot write bitmap font meta file:" << file.fileName();
            continue;
        }
        file.write(updatedContent.toUtf8());
        file.close();
        updated = true;
    }
    return updated;
}
