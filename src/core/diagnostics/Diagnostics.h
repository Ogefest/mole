#pragma once

#include <QLoggingCategory>
#include <QStringList>

namespace mole {

/// Every background job, the same way: what started, what it ended as, how long
/// it took, and -- for the ones that move bytes -- how many actually moved.
/// Written by Task itself, so a task written next year is covered without
/// knowing this exists.
Q_DECLARE_LOGGING_CATEGORY(taskLog)

/// Every operation on every drive: which drive, what was asked, what came back
/// and how long it waited. Written by the wrapper each mount goes behind, so
/// local disk and a plugin's backend report identically.
Q_DECLARE_LOGGING_CATEGORY(driveLog)

/// One line per network transfer -- the address, the result, the byte counts,
/// the speed, and whether the connection was a fresh one.
Q_DECLARE_LOGGING_CATEGORY(networkLog)

/// libcurl's own running commentary, the SSH and SFTP conversation included.
/// Verbose by nature; the one that answers "what did the server do just before
/// it stopped sending".
Q_DECLARE_LOGGING_CATEGORY(curlLog)

namespace diagnostics {

    /// Turns on the detail asked for in the `MOLE_LOG` environment variable, a
    /// comma-separated list of `task`, `drive`, `net`, `curl`, or `all`.
    ///
    /// Every category is silent by default at debug level and audible at warning
    /// level, so a truncated download or a failed job leaves a line in the session
    /// log whether anyone asked for logging or not, while the running commentary
    /// costs nothing until it is wanted.
    ///
    /// Returns the names that were turned on, so the application can say what it is
    /// recording. An unrecognised name is reported and ignored.
    QStringList applyEnvironment();

} // namespace diagnostics
} // namespace mole
