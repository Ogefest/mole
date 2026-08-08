# ADR-0003: The menu has two kinds of entry, so it gets two sections

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

`Tools` had become the place everything went. Eleven entries, of two entirely
different kinds, in one list:

- *Preview this file*, *Terminal here*, *Add to set*, *Index this folder* — do
  something to the files in front of you and return you to the listing.
- *Analyse folder*, *Find duplicates*, *Bulk rename*, *Sync folders*, *Saved
  reports*, *Alerts*, *Scheduled jobs* — open a tab that is a tool you then work
  in.

Read as one list they are indistinguishable, so finding anything means reading all
of it, and every new feature made it worse. `Section` is also an extension point:
it is what a plugin picks when it contributes an entry, so leaving it as one bucket
guarantees plugins keep filling the same bucket.

## Decision

`MenuAction::Section::Tools` is replaced by two sections:

- **`Operations`** — does something to what is selected, or to the folder you are
  in when nothing is selected, and leaves you where you were. At most a small
  dialog first.
- **`Workflows`** — hands you a tool in a tab of its own, which you then work in.

The rule for deciding, in one question: **does the entry do something to the files
in front of you, or does it hand you a tool to work with?** The tie-break, for the
entries where both sound true: if it needs a tab of its own to be useful at all, it
is a `Workflow`.

Worked examples, because a rule with no examples is an argument waiting to happen.
*Bulk rename* is a `Workflow` — it acts on a selection, but what it opens is a
tool with rules and a preview, and it is useless without one. *Add to set* is an
`Operation` — the sets view is a workflow, but adding the selection to a set is one
act, finished when it is done. *Preview this file* is an `Operation`: it opens a
tab, but the tab is the answer, not a workbench.

Section order in the menu is `File`, `View`, `Operations`, `Workflows`,
`Bookmarks`, `Help`.

## Reason

The alternative was to keep one section and sort within it, using separators to
suggest the grouping. Rejected because a separator is not a name: it tells the
reader that a boundary exists without saying what is on either side of it, and a
plugin author choosing a `sortOrder` cannot be guided by it at all. The point of
this change is to give plugins a question they can answer.

Splitting into more than two was also considered — one section per feature family,
say — and rejected on the ground already written into the enum: a menu with eleven
top-level headings is not navigation, it is a search problem. Two new names take
the count to six.

The names themselves went through several rounds. `Tasks` was rejected because the
application already shows running background tasks in a strip and the word would
mean two things. `Selection` was rejected because *Terminal here* and *Index this
folder* act on the folder you are in, not on a selection. `Actions` was rejected as
saying nothing: every entry in a menu is an action.

## Consequences

- This is a source-breaking change for anything outside the tree that named
  `Section::Tools`, and deliberately not softened with a deprecated alias. An alias
  would let a plugin keep making the choice this ADR exists to force, and the SDK
  is young enough that the honest fix is to name the section you mean.
  [docs/WRITING_PLUGINS.md](../WRITING_PLUGINS.md) is updated with both.
- Every section that has no entries is skipped when the menu is built, so a build
  where a whole family of features is absent does not show an empty heading.
- The split has to be checked, not assumed: a test asserts the six sections come
  out in that order, and that entries land in the section they asked for. What no
  test can settle is whether a particular entry was filed correctly — that is what
  the rule and its worked examples above are for.
