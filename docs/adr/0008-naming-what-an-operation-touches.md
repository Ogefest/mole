# ADR-0008: A destructive dialog names what it is about to touch

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

Four dialogs stood between somebody and something irreversible, and all four asked
with a number: *"Permanently delete 2 items?"*, *"7 files, 2.4 GB"*, *"12 files at
the destination will be removed"*. A number confirms what the person already
believed. It cannot catch the case the dialog exists for — that the operation is
aimed at something other than what they think it is aimed at.

That case is not hypothetical here. Ticks survive navigating away and coming back,
the cursor is a target when nothing is ticked, and a selection made in one folder is
easy to still be carrying in another. Every one of those is a way to arrive at a
correct-looking count over the wrong files.

## Decision

**A dialog that destroys, overwrites or packs lists the entries by name.** Not a
count — the rows themselves, scrolling when there are more than fit.

**The list is one component, `TargetList.qml`, used by all of them.** Compressing had
grown its own copy first; a second hand-rolled list would have been the moment the
two started disagreeing about how to say the same thing.

**Each row is `{ name, isDir, detail }`.** `detail` is free text on the right, so
each dialog can show what actually matters to it: a size when deleting files, a full
location when deleting duplicates (where every name in a group is identical and the
location is the only thing telling them apart), a relative path when syncing.

**Folders claim no size.** A folder's own size says nothing about what is inside it,
and *"4 kB"* beside a tree of ten thousand files would be worse than an empty column.

**What is listed comes from the same call that performs the operation.** The pane's
targets are read once, through `FileListModel::targetEntries()`, which is now what
`targets()` is built from. Two functions answering "what is selected" is exactly how
a list and an action drift apart.

**The list is taken when the dialog opens, not bound live.** A refresh landing or a
watcher firing behind an open dialog must not change what pressing Yes means.

## Reason

The alternative — showing the first few and "and 5 more" — was rejected because the
one you cannot see is the one that matters. Scrolling costs nothing and hides nothing.

Live binding was rejected on the same ground the snapshot is taken for: a dialog is a
question, and a question whose subject changes while it is on screen is not one that
can be answered honestly.

Deleting is the operation with no second chance, so it is the one that carries the
strongest form of this: the count, the names, and a line saying a folder goes with
everything inside it.

## Consequences

- `FileListModel::targetEntries()` is the single definition of what an operation is
  aimed at; `targets()` returns the uris of exactly those rows.
- A new dialog that destroys something has a component to reach for, and no excuse
  for asking with a number.
- The duplicate and sync confirmations grew a `selectedDetails()` and a `deletions()`
  on their controllers. Sync lists only the deletions: a plan can be thousands of
  steps, and the part being agreed to here is the part that destroys something.
