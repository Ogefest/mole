#pragma once

#include <QDateTime>
#include <QString>

namespace mole {

/// How long ago, in words: "just now", "12 min ago", "3 h ago", "yesterday",
/// "40 days ago", "never" for a time nothing has recorded.
///
/// **One wording, because there were three.** Six places spelled this out --
/// this function, and the reports list, the browser's folder facts, the alerts
/// view, the schedule list and the repository band -- and they disagreed:
/// "minutes ago" against "min ago", "yesterday" in three of six, and one that
/// could say "in 4 hours". The same file's age therefore read two ways in
/// adjacent tabs. TODO.md named four of the sites, of which one had moved and two
/// had not, and two more copies arrived after it was written.
///
/// The wording chosen is the compact one the three most-seen places already used.
///
/// Here in `src/ui` rather than beside a plugin, because `RepositoryInfo` is in
/// `src/ui/models` and cannot see `src/plugins` -- so this is the lowest layer all
/// six callers can reach. See MOLE-403.
QString ageInWords(const QDateTime& when);

/// The same, in either direction, against a clock the caller supplies: "in a
/// moment", "in 12 min", "tomorrow", "in 3 days".
///
/// The schedule list needs both halves -- a job's last run is behind and its next
/// run is ahead -- and it needs to ask about a `now` of its own so a list of
/// twenty rows reads as one moment rather than twenty.
QString timeInWords(const QDateTime& when, const QDateTime& now);

} // namespace mole
