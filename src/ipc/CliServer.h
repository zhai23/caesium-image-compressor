#ifndef CLISERVER_H
#define CLISERVER_H

#include "CliProtocol.h"

#include <QObject>
#include <QSet>

#include <functional>

class QLocalServer;
class QLocalSocket;

// Listens on a per-user named pipe and dispatches JSON commands to the
// running application. All work happens on the main thread, so commands that
// touch the UI model/state keep the interface in sync for free.
class CliServer : public QObject {
    Q_OBJECT

public:
    explicit CliServer(QObject* parent = nullptr);
    ~CliServer() override;

    // Attempts to take ownership of the pipe name. Returns false when another
    // instance of the application is already listening.
    bool start();
    bool isListening() const;

    using Handler = std::function<CliResponse(const QString& method, const QJsonObject& params)>;
    void setHandler(const Handler& handler);

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void sendResponse(QLocalSocket* socket, int id, const CliResponse& response);

    QLocalServer* server = nullptr;
    QSet<QLocalSocket*> clients;
    QHash<QLocalSocket*, QByteArray> buffers;
    Handler handler;
};

#endif // CLISERVER_H
