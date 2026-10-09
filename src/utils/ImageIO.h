#pragma once

#include <QImage>
#include <QString>

namespace ImageIO {
QString format(const QString& path);
QSize size(const QString& path);
QImage read(const QString& path, bool autoTransform = true);
}
