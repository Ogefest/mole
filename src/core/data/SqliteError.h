#pragma once

#include <QString>

class QSqlError;

namespace mole::sqlite {

/// A failed SQLite write, in words that say what to do about it.
///
/// **Two stores report the same failures and neither could name them.**
/// `DelimitedStore` had a version of this that compared
/// `QSqlError::nativeErrorCode()` whole against `"5"` and `"6"`, and Qt reports
/// SQLite's *extended* code wherever SQLite gives one -- so `SQLITE_BUSY_SNAPSHOT`
/// (517), `SQLITE_BUSY_RECOVERY` (261), `SQLITE_BUSY_TIMEOUT` (773) and
/// `SQLITE_LOCKED_SHAREDCACHE` (262) all fell through to the driver's own
/// "database is locked Unable to fetch row". `IndexDatabase` said nothing at all
/// about it: every write failure there arrived as the driver's text behind a
/// context. See MOLE-306.
///
/// Which matters because the two are different things to do about. A locked
/// database is another connection holding the file, and waiting is the answer. An
/// I/O error or a full disk is not, and an import that reported one as the other
/// sent whoever read it looking in the wrong place entirely.
///
/// `busyTimeoutMs` is the connection's own timeout, because "still locked" is only
/// true once that has run out and the sentence says how long that was.
QString describe(const QSqlError& error, int busyTimeoutMs);

} // namespace mole::sqlite
