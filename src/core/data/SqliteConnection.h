#pragma once

#include <QHash>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

class QThread;

namespace mole::sqlite {

/// One SQLite file, opened once per thread that touches it.
///
/// Three places used to do this, three different ways. Two named the connection
/// by the calling thread's address in hexadecimal and one cached per thread; two
/// set `journal_mode`, `synchronous` and `busy_timeout` and looked at none of the
/// results; `CountTableRowsTask.h` referred to "`connectionNameFor()` in ..." as
/// though there were one of it. The drift cost three separate faults -- an index
/// that leaked a handle per expired pool thread, two previews of one database
/// sharing a connection and breaking each other, and a store silently running
/// without the WAL it was written to depend on. See MOLE-356.
///
/// **A connection belongs to the thread that opened it.** Qt says so, and SQLite
/// agrees; there is no sharing one across threads and no borrowing. So the
/// registry here is keyed by thread, and the name carries a token unique to this
/// object -- two readers of the same file get two connections rather than
/// silently becoming one, which is what an address-and-path name could not
/// promise.
///
/// **A connection is removed when its thread ends.** A pool thread that touched
/// the file and then expired used to leave its `QSqlDatabase` in Qt's registry
/// for the rest of the session, with its page cache and its `-shm` mapping, and
/// Qt printed "requested database does not belong to the calling thread" the
/// next time that address came round. The removal is hooked to
/// `QThread::finished` and captures nothing but the connection's name, so it
/// cannot outlive anything or reach back into an object that has gone.
class Connection
{
public:
    /// What the file is opened for and what it is held to.
    struct Settings
    {
        /// Opened through SQLite's URI form with `mode=ro`, which is the only
        /// way SQLite refuses a write outright rather than being asked not to.
        /// `immutable` is deliberately never set: it would promise the file
        /// cannot change, and another process may well be writing to it.
        bool readOnly = false;

        /// Refuse to open at all when the file will not run in WAL.
        ///
        /// `PRAGMA journal_mode=WAL` was executed and its answer thrown away.
        /// An index parked on a filesystem that will not have it -- a network
        /// mount named by `MOLE_INDEX_PATH` -- ran in rollback-journal mode
        /// instead, readers queued behind writers again, and the fault ADR-0065
        /// exists to prevent was back with nothing said. Saying so is the whole
        /// point: a promise that quietly stopped holding is worse than a
        /// refusal, because nobody looks for it.
        ///
        /// An in-memory database is exempt, because SQLite answers "memory"
        /// there and always will.
        bool requireWal = false;

        /// How long a statement waits for another connection before giving up.
        /// Nought leaves SQLite's default, which is not to wait at all.
        int busyTimeoutMs = 0;

        /// Applied to every connection, in order, after the standard block.
        /// A failure is reported the same way `journal_mode` is.
        QStringList pragmas;
    };

    Connection(QString path, Settings settings);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// This thread's connection, opening one if there is not one yet.
    ///
    /// Invalid when it could not be opened, and then `lastError()` says what
    /// the driver said -- which is the part that used to be dropped, leaving
    /// callers to report "No index connection for this thread: " with nothing
    /// after the colon.
    QSqlDatabase forCurrentThread() const;

    /// What went wrong the last time `forCurrentThread()` could not open one.
    QString lastError() const;

    /// Closes and removes every connection this object opened. Safe only once
    /// nothing can still be using them.
    void closeAll();

    const QString& path() const { return m_path; }

private:
    /// Runs the pragma block on a freshly opened connection. Returns the reason
    /// it is not usable, or an empty string.
    QString settle(QSqlDatabase& database) const;

    QString m_path;
    Settings m_settings;
    /// Unique to this object, so two readers of one file cannot collide.
    QString m_token;

    mutable QMutex m_registry;
    mutable QHash<QThread*, QString> m_connections;
    mutable int m_nextConnection = 0;
    mutable QString m_lastError;
};

} // namespace mole::sqlite
