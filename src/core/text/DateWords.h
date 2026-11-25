#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QTime>

namespace mole {

/// A moment shown to a person: "2026-03-14 09:31", in local time, on a 24-hour
/// clock, whatever the machine's locale. Empty for an invalid one.
///
/// **One form, because the locale's short form is ambiguous.** Every date used to
/// be `QLocale::ShortFormat`, which under `en_US` is "3/14/26 9:31 AM": a
/// two-digit year, so a file from 1926 and one from 2026 read the same, and a
/// month and a day that trade places between an American reader and a European
/// one. Year-month-day is the one order nobody misreads. A handful of places
/// already wrote it out by hand; now they all ask here.
///
/// Dates only. Numbers stay in the reader's locale -- this is not "Mole ignores
/// the locale". See ADR-0107 and MOLE-467.
QString dateTimeInWords(const QDateTime& when);

/// A day with no time to it: "2026-03-14".
QString dateInWords(const QDate& day);

/// A time of day on its own, for something that happened today: "09:31".
QString clockInWords(const QTime& time);

} // namespace mole
