#pragma once

#include <QSqlDatabase>
#include <QString>

namespace mole::test::sqlite {

/// Making a database write fail on purpose, with the error a test names, and
/// costing no wall-clock.
///
/// **Three things can lose an import's rows -- a locked database, a disk with
/// nothing left on it, and an I/O error -- and none of them could be arranged.**
/// MOLE-291 could fail a commit only by closing the transaction out from under
/// the store, which holds the behaviour but produces no particular failure, so
/// `DelimitedStore::describe()`'s wording for a locked database had never been
/// reached by anything. See MOLE-293.
///
/// **The route this takes is SQLite's own levers, not a driver wrapper.** The
/// ticket chose a test-only QSqlDriver forwarding to QSQLITE, and the bound it set
/// was the cost: a forwarding driver means a QSqlResult forwarded method by
/// method. It turns out not to be needed. Two of the three failures can be asked
/// for directly, on the connection under test, with nothing wrapped and nothing in
/// `src/` knowing this exists:
///
/// - **`SQLITE_BUSY`, at once.** A contended write waits out the connection's busy
///   timeout -- five seconds in the store, which is exactly the test that must not
///   be written. A connection told to stop waiting fails immediately instead, with
///   the same plain code 5 the timeout would have produced.
/// - **`SQLITE_FULL`.** `PRAGMA max_page_count` caps a database, and a write that
///   needs one more page fails with "database or disk is full" from SQLite itself.
///   The cap belongs to the connection rather than to the file, which is why it is
///   applied to the connection under test rather than written in beforehand.
///
/// An I/O error is the one this does not reach, and a driver wrapper is still what
/// it would take.
///
/// The connection is found rather than injected: Qt keeps a registry, and the
/// store's connection is the open one on that file. That is the seam -- MOLE-291
/// found it first, and it is here now because it is what all of this is built on.

/// The connection a store has open on this file, or an invalid one.
QSqlDatabase connectionOn(const QString& databasePath);

/// Stops a connection waiting for a lock, so a contended write fails at once with
/// `SQLITE_BUSY` instead of after the busy timeout. False if the pragma was
/// refused, which means the arrangement did not happen and the case should say so
/// rather than assert on whatever came next.
bool stopWaitingForLocks(QSqlDatabase& connection);

/// Caps the database this connection sees at `pages`, so the write that needs one
/// more fails with `SQLITE_FULL`. Three pages is enough to hold a small schema and
/// not enough for rows.
///
/// False when the cap did not take, and it will not go below the pages already in
/// use -- which is why a database with a schema of any size wants the call below
/// instead of a number somebody guessed.
bool capAt(QSqlDatabase& connection, int pages);

/// Caps the database at exactly what it already uses, so there is no room for
/// anything more. What a full disk looks like from inside SQLite, whatever the
/// schema happens to cost.
bool capAtCurrentSize(QSqlDatabase& connection);

/// Holds the write lock on a database for as long as it is alive, so any other
/// connection's write is refused. One writer at a time is the rule in WAL, which
/// is the mode the stores use.
class WriteLock
{
public:
    explicit WriteLock(const QString& databasePath);
    ~WriteLock();

    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;

    /// False when the lock could not be taken -- a case that carried on regardless
    /// would be asserting on a write nothing was stopping.
    bool isHeld() const { return m_held; }

private:
    QString m_name;
    bool m_held = false;
};

/// Makes a connection's read snapshot stale, so its next write is refused with
/// `SQLITE_BUSY_SNAPSHOT` -- an *extended* code, and the one a comparison against
/// "5" does not recognise.
///
/// What it arranges is the real sequence: this connection opens a transaction and
/// reads, another connection writes and commits, and what the first is looking at
/// is no longer the database. SQLite refuses its write **at once**, without
/// consulting the busy handler at all -- there is nothing to wait for, which is
/// exactly why "still locked, try again in a moment" is the wrong thing to tell
/// anybody about it.
///
/// The transaction is left open on purpose: the case wants the connection in that
/// state. Rolling it back is the caller's, or the store's -- and where the
/// connection was *already* in a transaction, which is what an import looks like,
/// this leaves that one alone.
class StaleReadSnapshot
{
public:
    /// `connection` is the one whose write should fail; `databasePath` is the file,
    /// so this can open a second connection of its own to move it on.
    StaleReadSnapshot(QSqlDatabase& connection, const QString& databasePath);
    ~StaleReadSnapshot();

    StaleReadSnapshot(const StaleReadSnapshot&) = delete;
    StaleReadSnapshot& operator=(const StaleReadSnapshot&) = delete;

    /// False when the sequence could not be arranged, which a case has to check:
    /// asserting on a write that nothing had made stale proves nothing.
    bool isStale() const { return m_stale; }

private:
    QSqlDatabase m_connection;
    QString m_moverName;
    bool m_began = false;
    bool m_stale = false;
};

/// SQLite's own numbers, so a case can say which failure it meant.
///
/// Qt reports the *extended* code where SQLite gives one -- 517 for
/// `SQLITE_BUSY_SNAPSHOT`, say -- so a comparison against these has to be made on
/// the low byte. Anything that reads `nativeErrorCode()` and compares it whole
/// sees a busy database as something it has no branch for.
constexpr int kBusy = 5;
constexpr int kLocked = 6;
constexpr int kFull = 13;
constexpr int kBusySnapshot = 517;

/// The primary code out of whatever the driver reported.
int primaryCode(const QString& nativeErrorCode);

} // namespace mole::test::sqlite
