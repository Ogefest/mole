# ADR-0107: Dates are shown as year-month-day, whatever the locale

- **Date:** 2026-11-25
- **Status:** Accepted

## Context

Every date Mole showed a person went through `QLocale`'s short format: the
listing's modified column, the facts in a preview's drawer, the analysis report,
the drive list's last check. Under `en_US` that format is `3/14/26 9:31 AM`.

Two things are wrong with it:

- **The year has two digits.** A file from 1926 and one from 2026 read the same.
  A file manager is exactly the tool somebody uses to look at old files.
- **The day and the month swap places between readers.** `3/4/26` is the third
  of April to a European and the fourth of March to an American. The same
  screenshot answers differently depending on who looks at it.

This came up as a test fault (MOLE-461): an assertion that a date contained
`2026` passed in CI, whose C locale keeps four digits, and failed on a
workstation running `en_US`. MOLE-463 made the tests locale-proof and left the
format alone. Separately, a handful of places had already written
`yyyy-MM-dd HH:mm` by hand: the duplicate groups, the reports list, the index
list, the alerts and the automation history. So the same file's date already
read two ways in adjacent tabs.

## Decision

- **A date shown to a person is `yyyy-MM-dd HH:mm`, in local time, on a 24-hour
  clock**: `2026-03-14 09:31`. A day with no time is `yyyy-MM-dd`, and a time of
  day on its own is `HH:mm`.
- **One formatter does it**, `core/text/DateWords.h` (`dateTimeInWords`,
  `dateInWords`, `clockInWords`), in core so that the listing, the plugins and
  the drive list can all reach it. QML does not format dates. A view that needs
  one is given the text by C++, the way the search list's "scanned …" tooltip is
  now.
- **Only dates.** Numbers stay in the reader's locale, so "3,400 files" is still
  "3.400 Dateien" in German. Nor does this decide that Mole ignores the locale.
- Dates written to files (schedules, file sets, alert rules, the session log)
  were ISO already and are not affected.

## Reason

The alternatives were:

- **The locale's long format.** It keeps four digits, but it is long and it is
  still in the locale's order. Under `en_US` it spells out the weekday and the
  month. A listing column cannot afford that.
- **The locale's short date with a four-digit year forced in**, by editing the
  format string the locale returns. This keeps each reader's familiar order, and
  it was the closest contender. It still gives two readers different answers for
  one screenshot, and patching a locale's format string to replace `yy` is
  fragile across the locales Qt ships.
- **A setting.** A choice between formats is a choice somebody has to find, and
  the answer would differ between the guide's pictures and the person reading
  them. Nobody has asked for one. If somebody does, the one formatter is the
  place it would go.

Year-month-day is the one order nobody misreads (ISO 8601). It sorts as text, and
it is the form Mole already used for several of its dates.

## Consequences

- An American reader sees `2026-03-14 21:05` rather than `3/14/26 9:05 PM`. This is
  a deliberate cost of having one answer.
- A new place that shows a date calls `dateTimeInWords`. A `QLocale::ShortFormat`
  or `LongFormat` applied to a date in `src/` is a mistake, and a grep finds it.
- The guide's pictures that show dates were retaken in the new form.
