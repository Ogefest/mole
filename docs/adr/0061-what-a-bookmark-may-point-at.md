# ADR-0061: What a bookmark may point at

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

A bookmark was a name and a uri, and `bookmarks.json` an array of those two
fields. That is exactly enough to remember a folder — on disk, inside a mounted
archive, on a network share — and nothing else.

A set is the other thing in Mole somebody builds by hand and comes back to, and
it has no uri. So a set could be reached only by opening the Sets tab and picking
it out of a list, while a folder was one row in the sidebar. The request was to
make a set bookmarkable like a folder, which means the file format has to say
what a row points at.

## Decision

**A bookmark says what kind of thing it points at, and a set is named by its
id.**

```json
[
  { "name": "photos", "uri": "file:///home/u/photos" },
  { "kind": "set", "name": "Reading list", "setId": "8f2c…" }
]
```

- **`kind` absent means a folder.** Every file written before this change loads
  with every row a folder, unchanged.
- **A set's id goes under its own key, `setId`, never under `uri`.** An older
  Mole reading a newer file finds no `uri` on that row and skips it, which is the
  behaviour we want: it must not treat a set id as a path.
- **A row whose `kind` nobody recognises is skipped**, for the same reason. A
  format that grows is a format that gets read by code that predates the growth.
- **Identity is the pair.** Two bookmarks are the same bookmark when their kind
  and their target agree, so bookmarking one set twice gives one row, exactly as
  bookmarking one folder twice already did.
- **A set bookmark carries no copy of the set.** Its name and whether it still
  exists are read from `FileSetStore` every time they are asked for. Rename a set
  and the bookmark follows, with nothing polling: the store already emits
  `setsChanged`, and the model re-emits `dataChanged` for its set rows.
- **The name in the file is the last name seen**, refreshed whenever the store
  says something changed, and shown only when the set is gone.
- **A bookmark whose set has been deleted is marked dead, not dropped.** The
  model says so and the sidebar shows it greyed.
- **A set bookmark has no uri, and the model says so** — `UriRole` is empty for
  it, `TargetRole` carries the id. A consumer that only understands uris gets
  nothing rather than something that looks like a path.

## Reason

**Why not a `set:` pseudo-scheme in the existing `uri` field.** It is genuinely
tempting: all three consumers — the sidebar row, the Bookmarks menu entry and the
command palette — would keep passing one string, and nothing would need a second
field. It loses on two counts. A drive's uri scheme is derived from its name and
uniqueness is checked only against other drives, so a drive somebody calls "set"
would answer to the same scheme; and `VfsUri` would have to accept something that
is not a location, which is the beginning of every uri type that means four
different things.

**Why the id and not the name.** A name is what somebody typed and will change.
Referencing by id means renaming a set in the Sets tab renames it in the sidebar,
with no second copy to keep in step and no bookmark left pointing at a name
nothing answers to.

**Why a dead bookmark is kept rather than dropped on load.** The same reasoning
as a folder bookmark whose path has gone, which has always been kept: deciding
that a bookmark has stopped being useful belongs to the person who made it. A
loader that quietly removes rows is a loader that loses a bookmark to a drive
that happened to be unplugged.

**Why the last name seen is written down at all**, given that the live name is
read from the store. Because a dead bookmark has to say *something*, and "the set
this pointed at" is not a name. The stored name is a gravestone, not a cache: it
is never preferred over the store's answer while the set exists.

**Why a separate `addSet()`/`containsSet()`/`removeSet()` rather than one
kind-taking call.** The folder half of the API is called from three places and
tested by six assertions, all of which mean "a folder". Leaving those signatures
alone means the set half is additive, and a caller cannot pass a set id to
something that will treat it as a uri because there is no signature that would
take it.

## Consequences

- `bookmarks.json` gains one optional key and one new key. Both directions of
  compatibility hold, and there is a test for each.
- The sidebar, the Bookmarks menu and the palette have to decide what to *do*
  with a set row; that is MOLE-208, and none of it changes this format.
- A third kind — a saved search, a report — is now one enum value, one key and
  one branch in the loader, rather than a format change.
- `BookmarkModel` depends on `FileSetStore`. It is handed one rather than finding
  one, and a null store means set bookmarks simply read as dead, which is what a
  test that only cares about folders wants.
