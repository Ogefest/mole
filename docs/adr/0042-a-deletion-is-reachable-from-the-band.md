# ADR-0042: A listing shows what is on disk, so a deletion is reachable from the band

Date: 2026-08-18

Status: accepted

## Context

[ADR-0041](0041-git-state-is-read-through-libgit2.md) put git state in the window,
and MOLE-104 put a mark on the rows: `M`, `A`, `D`, `??`, `R`, `U`, git's own
letters, one per row.

Five of those six land on a row somebody can see. `D` does not. A listing shows
the folder as it is on disk, and a file git has been told to delete is not on
disk — there is no row to put the letter on. What the window says today is honest
but partial: the band counts the deletion in `3 changed`, and the folder that
held it carries the roll-up dot, so a deletion is visible one level up. The file
itself is absent, and there is no way to find out its name without leaving Mole
for a terminal.

## Decision

**A listing goes on showing what is on disk. The deletion becomes reachable from
the band instead.** No row is synthesised for a file that is not there.

Concretely: `RepositoryInfo` carries the paths git itself reported, the band's
`N changed` is something to open rather than only to read, and activating one of
its entries goes to that file the way anything else in Mole goes to a file — with
a deleted one going to the folder that held it and leaving the cursor on nothing.

## Why not a row of its own

The machinery for a synthesised row exists and would have been used:
`FileListModel::Provenance` already tells a row seen now from a row a scan
remembered, and [ADR-0038](0038-a-mixed-list-says-which-rows-are-remembered.md)
gave rows that are not on disk a path of their own. So the argument against it is
not that it would be hard to draw.

It is that a row is not only somewhere to put a letter. It is something a reader
can put a cursor on, tick, sort, filter, and press `F5` or `F8` at, and every one
of those needs an answer for a file with no bytes behind it. Six answers, and a
listing that can be asked to copy something that does not exist, is a steep price
for one letter — and the letter would be undoing on screen a deletion somebody
made on purpose. The cheapest of those answers is "it does nothing", and a row
that silently ignores half the keys the others obey teaches a reader that the
listing is unreliable rather than that this row is special.

Making the count reachable costs almost nothing next to that, because the paths
are already read. `RepositoryStatus::byPath` holds every one of them with its
flags, and `RepositoryContainsChanges` is the bit that separates a directory this
walk rolled up from a path git itself reported. Nothing new is walked and nothing
new is cached. It also composes: a list of changed paths is a list of uris, which
is what every operation in Mole already takes.

## Where the cursor goes

Going to a present file is `revealFile()`, unchanged. Going to a deleted one is
`revealMissingFile()`, and the difference is one row: the folder is the true half
of the answer and the cursor is the false half. Dropping the cursor on whatever
happens to sort first in that folder would be pointing at a different file while
looking exactly like success, so a reveal of something not expected to be there
lands with no row selected. It is also the folder already carrying the roll-up
dot, so the two halves of the feature agree about where a deletion lives.

## Consequences

- A deletion is nameable inside Mole. The count is the door, and it is the only
  door — which is why the count and the list have to agree, and why a test asserts
  they do against a work tree carrying one of each of the six states.
- The band is no longer only text. It stays a strip of facts: the list is a popup
  that a reader opens and closes, not height taken off the listing for as long as
  a checkout is dirty.
- Nothing here writes, stages, restores, or offers to. This sits inside the
  read-only boundary ADR-0041 draws around the whole feature.
- Reversing this means answering the six questions above, not writing a delegate.
