#ifndef CLIPATHCONFIG_H
#define CLIPATHCONFIG_H

#include <QString>

// Helper to manage the "caesium-cli" command on the per-user PATH environment
// variable.
//
// Instead of adding the whole application folder to PATH (which pollutes PATH
// with every file next to the executable and leaves stale entries behind when
// the app is moved), we create a small dedicated folder, e.g.
//   %LOCALAPPDATA%\CaesiumImageCompressor\bin
// containing a single "caesium-cli.cmd" wrapper file, and add ONLY that folder
// to PATH. The wrapper reads the real executable location from an "app-path.txt"
// pointer file that is rewritten every time the application starts, so moving
// the application to a new folder never requires re-adding to PATH and never
// leaves a stale entry from the previous location.
//
// On Windows the PATH value is stored as REG_EXPAND_SZ so any %VAR% entries
// already in PATH keep expanding. On other platforms the operations report
// "not supported".
namespace CliPathConfig {

bool isSupported();
// The single-file bin folder that is actually added to PATH.
QString binDir();
// Kept for callers: returns the folder that lives on PATH (== binDir()).
QString cliFolder();
// Full path of the "caesium-cli.cmd" wrapper inside binDir().
QString cliWrapperPath();
// Full path of the real executable that serves CLI commands, resolved from the
// "app-path.txt" pointer file (falling back to the current executable). This is
// the actual CLI binary location, not the PATH wrapper folder.
QString currentCliPath();
// Rewrites the pointer file so an existing wrapper keeps working after the app
// was moved to a new folder. Call at application startup.
bool updateAppLocation();
bool isConfigured();
bool addToPath();
bool removeFromPath();
// Starts the application detached so it runs in the background. On Windows the
// child does not inherit the terminal's std handles, so CLI pipelines do not
// hang waiting for EOF while the GUI stays open.
bool launchAppDetached();

} // namespace CliPathConfig

#endif // CLIPATHCONFIG_H
