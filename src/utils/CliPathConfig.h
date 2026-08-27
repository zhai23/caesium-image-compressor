#ifndef CLIPATHCONFIG_H
#define CLIPATHCONFIG_H

#include <QString>

// Helper to add/remove the Caesium application folder (the one containing
// "Caesium Image Compressor.exe" and "caesium-cli.cmd") from the per-user PATH
// environment variable. On Windows the value is stored as REG_EXPAND_SZ so any
// %VAR% entries already in PATH keep expanding. On other platforms the
// operations report "not supported".
namespace CliPathConfig {

bool isSupported();
QString cliFolder();
QString cliWrapperPath();
bool isConfigured();
bool addToPath();
bool removeFromPath();
// Starts the application detached so it runs in the background. On Windows the
// child does not inherit the terminal's std handles, so CLI pipelines do not
// hang waiting for EOF while the GUI stays open.
bool launchAppDetached();

} // namespace CliPathConfig

#endif // CLIPATHCONFIG_H
