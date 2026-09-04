# ADR-0032: A feature says whether a new tab of it means anything

- **Date:** 2026-08-10
- **Status:** Accepted

## Context

The File menu offered a *New … tab* for every registered feature, minted in a loop
over the registry. Thirteen features, thirteen entries, and they were not all alike:

- **New Preview tab** opened a preview of no file and showed nothing.
- **New Duplicates tab** opened a view whose own first line reads *"Open this from a
  folder to search it."* — it told the user they had arrived the wrong way.
- **New Sync tab** opened with neither endpoint set.
- **New Bulk rename tab** and **New Analysis tab** had nothing to work on.

Meanwhile most of those same features already had a proper entry elsewhere that
arrived carrying something to work on — *Preview this file*, *Find duplicates*,
*Sync folders* — so the menu held both: a verb that worked and a noun that did not.

Nothing was wrong with the loop. The problem was that **nothing in the loop could ask
whether a feature is the kind you open from nothing**, so it had to assume all of them
were.

Two methods on `IFeature` were already documented for questions in this area and read
by nobody who needed them. `needsContext()` said *"the shell uses it to avoid offering
a tab that would open onto nothing"* and only the empty-window button row read it;
`sortOrder()` was documented as *"ordering hint for the new-tab menu"* and the menu
used registration order instead. Two of the ingredients existed. That is worth
recording, because a declared-but-unread method is a promise the code is not keeping,
and this is the third time one has turned up.

## Decision

**`IFeature::opensFromNothing()` decides whether the File menu offers a *New … tab*.**
It is the feature's answer about itself, in the registry, because the menu has no way
to know and should not be taught a list of exceptions.

There are three kinds of feature, and one predicate separates the one from the two:

- **Opens from nothing** — a browser, a search. You open several, from cold, as a
  matter of course. `true`. These get *New … tab*.
- **Needs a subject** — a preview needs a file, a bulk rename a selection, a duplicate
  scan its roots, a sync two endpoints, an analysis a folder. `false`, and they also
  answer `needsContext()` — which is a different question, about whether a tab would
  be *empty*, and is what the empty window reads to decide which buttons to offer.
- **A standing tool that exists once** — the alerts list, the saved reports, the
  schedule, the sets, and the list of indexes. `false`. Opening a second tab of one is
  a duplicate of the first, and the entry reads better as its own name in the
  Workflows section than as something being created: *Saved reports*, not *New Reports
  tab*. ADR-0003 already put those in Workflows.

  **Amended 2026-08-21 (MOLE-259): the list of indexes is the fifth.** It was not left
  out on purpose — this record was written on 2026-08-10 and the Indexes tab arrived on
  2026-08-20, so the list of four is a list written before the fifth existed. It is the
  same shape as the alerts list and the schedule: one store, everything in it listed,
  actions offered on the rows, and a second one is the same duplicate with its own idea
  of what is selected. Named here rather than derived from a predicate, for the reason
  `openStandingTab()`'s own comment gives: `opensFromNothing()` is the wrong test,
  because a duplicate scan and a bulk rename answer `false` too and reusing a
  Duplicates tab halfway through a scan would throw the scan away. So a sixth arriving
  is a line somebody has to add here on purpose, which is the same bargain the
  File-menu test below makes.

**The default is `false`.** A feature gets no *New … tab* entry unless it asks for one.

**A feature that answers `false` must be reachable another way** — its own menu action,
which is also what puts it in the command palette, since the palette is built from the
action registry. `MenuAction::opensFeature` names the feature an entry opens, and
`everyFeatureIsReachableFromTheMenu` in `tst_AppIntegration` fails, naming the feature,
when a registered one is named by no action at all.

**The order of the File section is `sortOrder()`,** finally read: the two browsers,
then the two searches. The test names the four in order rather than counting them, so a
fifth arriving is a line somebody has to add on purpose.

## Reason

**Why `false` as the default**, which is the part most likely to be questioned. The
other way round — a feature opens from nothing unless it says otherwise — keeps a
third-party plugin's menu entry working without its author knowing this method exists.
It lost on what each mistake costs:

- With `true` as the default, a wrong answer is *a menu entry that opens onto nothing*
  — precisely the fault being fixed here — and nothing would ever notice, because the
  honest answer is the one you have to remember.
- With `false`, a wrong answer is *a missing menu entry* for a feature that is still
  reachable from its own action, the palette and a restored session; and a plugin
  author who wants the entry says so in one line. The reachability test makes the
  worse version of that mistake — a feature reachable by nothing at all — a build
  failure rather than a discovery.

So the default is the one whose failure mode is visible and recoverable.

**Why a predicate rather than reusing `needsContext()`.** They answer different
questions and the answers differ for four features. The alerts list *can* open from
nothing — it is a perfectly good empty list — so `needsContext()` is correctly false
for it, and the empty window is right to offer it as something to open. But *New Alerts
tab* is still the wrong entry, because there is only ever one alerts list. Collapsing
the two questions into one would have meant either offering New Alerts tab or hiding
Alerts from the empty window, and both are wrong.

**Why not remove the Workflows entries instead** and keep the generated ones? Because
*Saved reports* and *Scheduled jobs* say what they are, and ADR-0003 already decided
that a tool you open and work in belongs in Workflows. The generated entry is the one
that was adding nothing.

## Consequences

- What a plugin author must do: answer `opensFromNothing()` with `true` for a tab kind
  people open from cold; otherwise register a menu action that opens it, with
  `opensFeature` set. Doing neither fails the suite by name.
- The File section holds four *New … tab* entries instead of thirteen. Nothing became
  unreachable: all nine of the others already had their own action.
- `needsContext()` is now true for Duplicates and Sync as well, which it should always
  have been — so the empty window stops offering two tabs that say, once opened, that
  they were opened the wrong way.
- `sortOrder()` is read for the first time. A plugin can slot between the built-ins,
  which is what the gaps in their numbers were always for.
- ADR-0003's Operations/Workflows split is untouched and now easier to state: the File
  section is what you open from nothing, Workflows is what you open onto something.

## Amendment, 2026-09-05: the shell may name a tool, and may not copy a key

This record accepts the shell naming a standing tool by id -- `openPreviewTab()`
asks for `mole.preview`, `openReportFor()` for `mole.analysis`, and the File
section is built by asking every feature whether it opens from nothing. That is
the arrangement here and it stands: an id is how the shell reaches something
without knowing what it is, which is what makes the feature a plugin rather than
a special case.

**What it does not cover, and what is now refused, is a second copy of something
QML declares.** `buildActions()` kept a hash of three feature ids to key text --
`mole.browser` to `Ctrl+T`, `mole.commander` to `Ctrl+Shift+T`, `mole.livesearch`
to `Ctrl+F` -- used for nothing but the label beside a menu entry, while the
accelerators that actually fire are `Shortcut` items in QML. Every other label was
a string in the same function. A copy is wrong the moment either side moves, and
it had been: MOLE-396 found five entries advertising a key
that was not bound or did something else, and one key printed beside two entries.

So the window declares each key for the thing it reaches --
`App.declareShortcut("mole.tools.analyse", this)` from the `Shortcut` that binds
it -- and the shell holds no key text at all. Two labels stay in `buildActions()`
and are not an exception: "type to filter" and "type to find, or /" are not keys,
nothing else spells them, and there is nothing for them to disagree with.

A member on `IFeature` was the other reading, and would make a shortcut something
a plugin can claim. It was dropped for what it costs against nothing yet asking
for it: an SDK change, a `kPluginApiVersion` bump, and a rule about two plugins
claiming one key. See MOLE-416.
