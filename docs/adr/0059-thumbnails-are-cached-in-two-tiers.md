# ADR-0059: Thumbnails are cached in two tiers, and the disk one is a cache directory

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

A `GridView` destroys a delegate that leaves its cache buffer and builds a new one
when it comes back. So without a cache, every visit to a folder of photographs
decoded every photograph again — and so did every scroll back up the same folder.
On a folder of five hundred that is the difference between a view that feels
instant and one that flickers grey squares for ever.

## Decision

**Two tiers, because they answer two different questions.**

**In memory, for the scroll.** A bounded store of decoded thumbnails, capped in
**bytes** rather than in count: a folder of 4K panoramas and a folder of icons
must not have wildly different footprints. 32 MB, which holds a few hundred tiles
— the gesture this exists for is a scroll to the bottom of a folder and back.
Least recently used goes first. Asked on the UI thread, because it does no I/O.

**On disk, for the next visit.** Encoded thumbnails in a directory, written after
a successful decode and read before one is attempted. 256 MB, which holds a few
thousand tiles. Asked only from a worker thread, because reading it is I/O and the
UI thread never touches storage — which is why the two tiers are separate calls
rather than one `lookup()`.

- **`QStandardPaths::CacheLocation`, not `AppDataLocation`.** Ten stores use the
  latter and every one of them holds something that cannot be recomputed. This is
  the first that can, and deleting it must cost nothing but time.
- **`MOLE_THUMBNAILS_PATH` overrides it**, exactly as the other stores'
  variables do, and `PrivateProfile` sets it — a test that writes into the real
  cache directory is a test that changes the machine it runs on.
- **The key is the uri, the requested size and the source file's modification
  time**, hashed to 128 bits so a name is short and the directory stays flat. The
  date is what makes an edited photograph produce a new thumbnail rather than the
  old one, and the size is there because the same file in the small grid and in
  the gallery are two different pictures. Each entry records the uri it came from
  in the image's own text field, so a file in that directory can be explained.
- **JPEG where there is no transparency, PNG where there is.** A cache costing
  200 kB an entry is a cache nobody wants; a transparent thumbnail flattened onto
  black is a wrong picture rather than a smaller one.
- **A cap, and eviction on write.** Least recently *read* first, where "read" is
  stamped on the cache file's own modification time — so the directory is the
  state and the cap holds across a restart with nothing to reload. A cache with no
  ceiling is a bug report in six months, and the sizes here are unbounded: a
  hundred thousand pictures is a hundred thousand entries.
- **A corrupt or truncated entry is a miss**, not a crash and not a broken tile.
  It is a directory anybody can delete half of.

**Two requests for one picture are one decode.** The pump keys work in flight by
the thumbnail key rather than by the request, so two panes showing the same folder
wait on one task. Cancelling one of them cancels the decode only when nobody is
left waiting.

## Reason

The alternatives, and what disqualified them:

- **Only the memory tier.** It makes the scroll instant and the *second visit*
  exactly as slow as the first, which on a folder somebody lives in is the case
  that matters more.
- **Only the disk tier.** Every scroll back up a folder becomes a read and a
  decode of the stored JPEG. Cheaper than decoding the original, still work for
  something already in hand a moment ago.
- **An index file for the disk tier's sizes and read times.** It has to be
  rewritten on every read to be true, and it can disagree with the directory —
  after a crash, or after somebody deletes half of it by hand. The file's own
  modification time cannot disagree with the file.
- **`AppDataLocation`, beside the other stores.** It would put something
  recomputable where everything else is not, and the first person to back up their
  profile would back up a quarter of a gigabyte of thumbnails.
- **Keying on a content hash instead of the date.** It reads the file to decide
  whether to read the file, which is backwards on the remote drives MOLE-143 has
  to be careful on.
- **Capping in entries rather than in bytes.** Two thousand icons and two thousand
  panorama tiles are two orders of magnitude apart, and only one of those numbers
  is a memory footprint.

## Consequences

The cache is owned by the image provider, so there is one per window and every
pane shares it — which is what makes the coalescing worth having.

`ThumbnailCache` is asked in two places for one lookup, and a caller that asks
the disk tier from the UI thread would break the house rule with no compiler to
stop it. The header says so on both methods; the pump is the only caller.

Eviction is checked on write and never swept, so a cache left alone shrinks
never. That is deliberate: a sweep needs something to trigger it, and nothing here
wants a timer.
