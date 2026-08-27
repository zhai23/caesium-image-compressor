#ifndef CLICLIENT_H
#define CLICLIENT_H

#include "CliProtocol.h"

// Blocking client used by the "second instance" (the CLI invocation) to talk
// to the already-running application through the named pipe.
namespace CliClient {

CliResponse sendRequest(const QString& method, const QJsonObject& params, int timeoutMs = 60000);
bool isRunning();

} // namespace CliClient

#endif // CLICLIENT_H
