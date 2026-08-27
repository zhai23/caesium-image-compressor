#include "CliClient.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

CliResponse CliClient::sendRequest(const QString& method, const QJsonObject& params, int timeoutMs)
{
    CliResponse response;

    QLocalSocket socket;
    socket.connectToServer(CliProtocol::pipeName());
    if (!socket.waitForConnected(3000)) {
        response.ok = false;
        response.code = QStringLiteral("NOT_RUNNING");
        response.message = QStringLiteral("Caesium Image Compressor is not running.");
        return response;
    }

    QJsonObject request;
    request[QStringLiteral("id")] = 1;
    request[QStringLiteral("method")] = method;
    request[QStringLiteral("params")] = params;
    socket.write(CliProtocol::encodeFrame(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    socket.flush();

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (socket.waitForReadyRead(100)) {
            buffer.append(socket.readAll());
            QByteArray payload;
            while (CliProtocol::tryDecodeFrame(buffer, payload)) {
                const QJsonDocument doc = QJsonDocument::fromJson(payload);
                if (!doc.isObject()) {
                    continue;
                }
                const QJsonObject resp = doc.object();
                if (resp.value(QStringLiteral("id")).toInt(-1) == 1) {
                    response.ok = resp.value(QStringLiteral("ok")).toBool();
                    if (response.ok) {
                        response.result = resp.value(QStringLiteral("result")).toObject();
                    } else {
                        const QJsonObject error = resp.value(QStringLiteral("error")).toObject();
                        response.code = error.value(QStringLiteral("code")).toString();
                        response.message = error.value(QStringLiteral("message")).toString();
                    }
                    return response;
                }
            }
        }
        if (socket.state() == QLocalSocket::UnconnectedState) {
            break;
        }
    }

    response.ok = false;
    response.code = QStringLiteral("TIMEOUT");
    response.message = QStringLiteral("Timed out waiting for Caesium to respond.");
    return response;
}

bool CliClient::isRunning()
{
    return sendRequest(QStringLiteral("ping"), {}, 2000).ok;
}
