#ifndef CLIRUNNER_H
#define CLIRUNNER_H

#include <QStringList>

// Human-friendly CLI front-end. Invoked by a second instance of the
// executable ("caesium --cli <command> ..."); connects to the running app,
// sends the command, prints the result and returns a process exit code.
namespace CliRunner {

int run(const QStringList& args);

} // namespace CliRunner

#endif // CLIRUNNER_H
