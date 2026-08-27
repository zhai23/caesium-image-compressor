#include "CliServer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

CliServer::CliServer(QObject* parent)
    : QObject(parent)
    , server(new QLocalServer(this))
{
}

CliServer::~CliServer() = default;

bool CliServer::start()
{
    if (server->isListening()) {
        return true;
    }
    // Remove a stale pipe left behind by a previous crashed instance. If a
    // live instance owns the name, removal fails and listen() also fails.
    QLocalServer::removeServer(CliProtocol::pipeName());
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(CliProtocol::pipeName())) {
        return false;
    }
    connect(server, &QLocalServer::newConnection, this, &CliServer::onNewConnection);
    return true;
}

bool CliServer::isListening() const
{
    return server->isListening();
}

void CliServer::setHandler(const Handler& h)
{
    handler = h;
}

void CliServer::onNewConnection()
{
    while (server->hasPendingConnections()) {
        QLocalSocket* socket = server->nextPendingConnection();
        buffers.insert(socket, QByteArray());
        clients.insert(socket);
        connect(socket, &QLocalSocket::readyRead, this, &CliServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            buffers.remove(socket);
            clients.remove(socket);
            socket->deleteLater();
        });
    }
}

void CliServer::onReadyRead()
{
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    buffers[socket].append(socket->readAll());

    QByteArray payload;
    while (CliProtocol::tryDecodeFrame(buffers[socket], payload)) {
        int id = -1;
        CliResponse response;

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            response.ok = false;
            response.code = QStringLiteral("PARSE_ERROR");
            response.message = QStringLiteral("Invalid JSON request.");
        } else {
            const QJsonObject request = doc.object();
            id = request.value(QStringLiteral("id")).toInt(-1);
            const QString method = request.value(QStringLiteral("method")).toString();
            const QJsonObject params = request.value(QStringLiteral("params")).toObject();
            if (method.isEmpty()) {
                response.ok = false;
                response.code = QStringLiteral("UNKNOWN_METHOD");
                response.message = QStringLiteral("Missing 'method' in request.");
            } else if (!handler) {
                response.ok = false;
                response.code = QStringLiteral("NO_HANDLER");
                response.message = QStringLiteral("Server has no command handler.");
            } else {
                response = handler(method, params);
            }
        }
        sendResponse(socket, id, response);
    }
}

void CliServer::sendResponse(QLocalSocket* socket, int id, const CliResponse& response)
{
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("ok")] = response.ok;
    if (response.ok) {
        json[QStringLiteral("result")] = response.result;
    } else {
        QJsonObject error;
        error[QStringLiteral("code")] = response.code;
        error[QStringLiteral("message")] = response.message;
        json[QStringLiteral("error")] = error;
    }
    socket->write(CliProtocol::encodeFrame(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    socket->flush();
}
