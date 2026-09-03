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

/// What every store on disk could not do. A configuration file that could not be
/// written, and one that could not be read and was kept rather than replaced.
///
/// Warnings only, in practice: a store saying nothing is the ordinary case, and
/// a store saying something is a change the user has already been shown and the
/// disk has not taken. See ADR-0089.
Q_DECLARE_LOGGING_CATEGORY(storeLog)

/// The one request Mole makes on its own account: whether a newer release
/// exists. Everything that check decides is written here and nowhere else,
/// because the whole of its design is that it never says anything out loud --
/// so when somebody asks why they were or were not told about a version, this
/// is the only place with an answer.
Q_DECLARE_LOGGING_CATEGORY(updateLog)

namespace diagnostics {

    /// Turns on the detail asked for in the `MOLE_LOG` environment variable, a
    /// comma-separated list of `task`, `drive`, `net`, `curl`, `update`, or `all`.
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
