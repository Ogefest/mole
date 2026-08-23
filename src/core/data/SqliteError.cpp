#include "core/data/SqliteError.h"

#include <QSqlError>

namespace mole::sqlite {

namespace {

    /// SQLite's codes, by the names its own documentation uses.
    constexpr int kBusy = 5;
    constexpr int kLocked = 6;
    constexpr int kBusySnapshot = 517;

} // namespace

QString describe(const QSqlError& error, int busyTimeoutMs)
{
    bool numeric = false;
    const int reported = error.nativeErrorCode().toInt(&numeric);
    if (!numeric)
        return error.text();

    // The snapshot case first, because it is the one the sentence below would be
    // wrong about. Nothing waited and nothing is holding the file: another
    // connection wrote while this one had a read open, so what this connection
    // was looking at is no longer the database. The answer is to start again, not
    // to wait -- and being told to wait is a person watching a progress bar that
    // will never move.
    if (reported == kBusySnapshot) {
        return QStringLiteral("Another connection changed the database while this one was reading it, "
                              "so the write could not be applied -- it has to be tried again");
    }

    // Everything else in the busy and locked families, by the low byte, because
    // that is where SQLite keeps the primary code and Qt hands back the extended
    // one whenever there is one.
    switch (reported & 0xff) {
    case kBusy:
    case kLocked:
        return QStringLiteral("The database was still locked by another connection after %1 seconds")
            .arg(busyTimeoutMs / 1000);
    default:
        return error.text();
    }
}

} // namespace mole::sqlite
