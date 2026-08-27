#ifndef CLIPROTOCOL_H
#define CLIPROTOCOL_H

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonObject>
#include <QString>
#include <QtEndian>

// A structured response returned by the running application for every command.
struct CliResponse {
    bool ok = true;
    QString code;
    QString message;
    QJsonObject result;
};

namespace CliProtocol {

constexpr quint32 kMaxFrameSize = 64 * 1024 * 1024;

// Per-user pipe name so different OS users never collide.
inline QString pipeName()
{
    const QString key = QCoreApplication::organizationName()
        + QStringLiteral("/") + QCoreApplication::applicationName()
        + QStringLiteral("/") + QDir::homePath();
    const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QStringLiteral("caesium_") + QString::fromLatin1(hash);
}

// 4-byte big-endian length prefix + payload.
inline QByteArray encodeFrame(const QByteArray& payload)
{
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), reinterpret_cast<uchar*>(frame.data()));
    frame.append(payload);
    return frame;
}

// Extracts one complete frame from the buffer (if any).
inline bool tryDecodeFrame(QByteArray& buffer, QByteArray& payload)
{
    if (buffer.size() < 4) {
        return false;
    }
    const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData()));
    if (length > kMaxFrameSize) {
        // Corrupt stream: drop everything.
        buffer.clear();
        return false;
    }
    if (buffer.size() < 4 + static_cast<int>(length)) {
        return false;
    }
    payload = buffer.mid(4, static_cast<int>(length));
    buffer.remove(0, 4 + static_cast<int>(length));
    return true;
}

} // namespace CliProtocol

#endif // CLIPROTOCOL_H
