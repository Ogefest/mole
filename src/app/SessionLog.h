#pragma once

#include <QString>

namespace mole::sessionLog {

/// Writes everything the application says to a file, as well as to the console.
///
/// A crash takes the terminal's scrollback with it, and the interesting lines
/// are usually the ones printed before the fall. Every line is flushed as it is
/// written, so a log survives a segmentation fault -- a buffered log would lose
/// exactly the part worth reading.
///
/// The previous session is kept alongside the current one, because the usual
/// way to notice a crash is to start the program again, and starting it again
/// is what would otherwise overwrite the evidence.
///
/// When the program does fall over, the signal handler writes the backtrace
/// into the same file before letting the process die as it would have.

/// Starts logging. Returns the path being written to, empty if it could not be
/// opened -- in which case the console still gets everything, as before.
QString install();

/// Where the log goes. `MOLE_LOG_PATH` overrides it; otherwise it sits beside
/// the rest of the profile, so backing up the configuration keeps it.
QString defaultPath();

/// Stops logging and closes the file. Called at exit; safe if never installed.
void shutdown();

} // namespace mole::sessionLog
