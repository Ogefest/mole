# ADR-0037: The scope is a field, not a second tab

- **Date:** 2026-08-11
- **Status:** Accepted
- Supersedes the *index-search tab stays as it is* paragraph of
  [ADR-0005](0005-which-engine-answers-a-search.md); the rest of that record stands.

## Context

ADR-0005 joined the two searches half way. The `Ctrl+F` form learned to ask the
index when an indexed root covers the folder, and to say which engine answered.
It kept the second tab, and said why:

> The index-search tab stays as it is. It answers a different question — *find
> this across every volume I have indexed* — and folding it into the folder
> search would lose that.

The question was real. The tab was not the only way to keep it. What the user
met was two forms, two result views and two ideas of what a search is, with the
difference between them being which engine answered — an implementation detail
wearing a keyboard shortcut. `Ctrl+Shift+I` had no name in the interface that
said *everywhere*; it said *Indexed search*, which is a statement about
machinery.

Since MOLE-147 both engines take the same query, so the second tab no longer even
had a technical reason to exist.

## Decision

**One search. Where to look is a field on it.**

*Search in* offers the folder the search was opened from, a path typed or chosen,
or **everywhere indexed** — with the volume picker, its per-volume counts and the
*Scan a folder…* button beside it, all of which were the retired tab's.

- `Ctrl+F` is unchanged: a box, a name, Return.
- `Ctrl+Shift+I` opens the same search with the scope preset to everywhere
  indexed. A preset, not a second tool.
- The scope decides the engine when it is *everywhere* — a walk of every volume
  anybody ever scanned is not something to wait for. For a folder, ADR-0005's
  rule is untouched: the index when it covers the subtree and the toggle is on, a
  walk otherwise. The *Use the index* toggle is hidden for the everywhere scope,
  because there is nothing there to turn off.
- `IFeature` gains **`absorbedIds()`**: the ids a feature has taken over. A
  session restore maps a retired id through it, so a tab saved before the merge
  reopens as its successor rather than as nothing.

## Reason

**A scope is what the user was choosing all along.** Nobody opens the indexed
search because they want SQL; they open it because the thing they are looking for
is not in this folder. Naming the scope says that, and naming the engine did not.

**`absorbedIds()` on the feature rather than a table in the shell**, because the
fact belongs to the feature that did the absorbing. A table somewhere central
would have to be maintained by whoever merges two features and is read by code
that has no reason to know either of them — and the shell has never had to know
what a search is. The alternative, dropping the retired tabs, is the answer an
uninstalled plugin deserves and a merged one does not: silently losing a tab on
upgrade is the kind of thing that reads as the application forgetting.

**Rejected: keeping both tabs and cross-linking them.** That is two tools with a
signpost, and the signpost is an admission that the split was wrong.

**Rejected: making the everywhere scope a checkbox beside the path.** It is not a
modifier of a folder search — the path field is meaningless while it is on — so a
control that reads as a modifier would be describing something else.

## Consequences

- `IndexSearchController`, `IndexSearchFeature` and `IndexSearchView.qml` are
  gone. Everything they offered — the name box, the volume picker with its
  counts, the search button, the status line, the busy indicator, the scan dialog
  and its refresh-on-scan — is in the one form, which also lets the results
  become a file set, something the retired tab could not do.
- The File menu offers one *New search tab*, and ADR-0032's rule is satisfied
  without a second entry. `Ctrl+Shift+I` has no menu entry of its own on purpose:
  giving a preset its own line would put the two searches back in the menu.
- The guide's two sections become one. MOLE-157 owns that page; this change owns
  not leaving it describing a tab that is not there.
- Every plugin author gains `absorbedIds()`, defaulted to empty, so nothing
  outside this repository has to do anything.
