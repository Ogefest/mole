# ADR-0083: A mount can be a place without being a drive, and it is rebuilt from the uri

- **Date:** 2026-08-24
- **Status:** Accepted

## Context

`VfsManager.h` opens by saying what a mount is: *what the user perceives as a
"drive" in the sidebar*. That was true of every mount there was — a disk, a
configured server, a bucket — and it stopped being true the moment an archive could
be opened for browsing. `AppController::openArchive()` adds a mount, so opening a
`.zip` adds a sidebar row and leaves it there.

MOLE-301 multiplied it. Eleven more suffixes means a `.jar`, a `.deb`, an `.rpm`, a
`.whl` and the rest each leave a row, so a session spent looking inside a handful
of packages ends with a sidebar nobody asked for. The author asked for it to stop.

There was already one exception to *a mount is a drive*: `Mount::internal`, which
the preview uses to read a member out of a single compressed file. Its contract is
*a mount that exists so something can be read, not a place anybody can go* — it is
left out of anything that offers a drive, and whoever created it removes it again.

## Decision

**A third state: `Mount::unlisted`.** A place somebody is standing in that the
sidebar does not offer. An archive opened for browsing is one; the preview's member
mount stays `internal`.

**It goes away when the last reader leaves.** `AppController` recomputes who is
inside what at the two moments the answer can change — a tab moving, and a tab
appearing or going — and removes any unlisted mount that has been stood in and is
now empty.

**A uri into it rebuilds it.** `IFileSystemFactory::configForRoot()` is the other
direction of `rootUriForFile()`: given a root uri, the config that would mount it,
or nothing. `VfsManager::remountFor()` uses it, and every navigation goes through
`BrowserPaneController::navigateTo()`, which asks.

## Reason

**Not `internal`, and the difference is not pedantry.** Three things rest on that
flag: the preview relies on being removed by its own owner, `openArchive()`'s dedup
loop skips internal mounts on purpose so a preview in flight is never handed over
as a place to browse, and nothing internal is a place a person is. A browsable
archive breaks all three ways — it *is* somewhere a person is, and opening the same
zip twice must reuse the mount rather than make a second. Widening `internal` to
cover it would have made one flag mean two things, and the first bug would have
been a preview's mount being reused as a browsing destination while the preview
still held it.

**The lifetime is the substance, not the flag.** A mount nobody can see and nobody
removes is worse than the row it replaced: forty invisible mounts, each holding a
file handle and a cached central directory, with no way to eject one. The flag is
five lines; this is the work.

**"Nobody is inside it right now" is the wrong test, and it failed the first time
it was written.** A mount is created *before* anything navigates into it, and
navigation is queued — so the first version removed the mount between the two
halves of opening an archive: `openArchive()` mounted, a tab appeared, the tab's
first report arrived before it had moved, and the mount was gone by the time the
navigation asked for it. The rule is therefore *the last reader has left*: a mount
has to have been stood in once before it can be left. That also keeps the trigger
honest — pruning on `mountsChanged` is what made adding a mount remove it, and
adding a mount cannot change who is standing anywhere.

**The rebuild is what makes disappearance safe rather than a trap**, and it is
possible only because of a decision made earlier for a different reason:
`ArchiveFileSystem::authorityFor()` is the percent-encoded local path, so an
`archive://` uri carries the archive's own address. A bookmark, a back step or a
session restored tomorrow rebuilds the mount from what it already holds. Where the
file has gone, the failure is the ordinary one for a file that is not there.

`configForRoot()` returns nothing by default, and that is the honest answer for
almost every backend: an SFTP root names a host and no credential, and rebuilding
one from a uri would be guessing. Those mounts are configured and kept, so they
never need it.

The alternatives considered:

- **Keep the row and let people remove it.** What the author objected to.
- **A row that hides itself after a while.** A clock deciding what the sidebar
  shows, and a mount whose lifetime nobody can predict.
- **No mount at all — read archives through a special path.** Every reader in Mole
  reaches a file by resolving a uri, and a second way to reach one would be a
  second implementation of everything downstream.
- **Reference-counted mounts.** The bookkeeping the two triggers already do for
  free, plus a leak whenever a holder forgot to release. What is on screen is the
  reference count, and it is recomputed rather than maintained.

## Consequences

An archive opens, is walked around in, and leaves nothing behind. The sidebar means
*drives* again, which is what `VfsManager.h`'s first line says it means.

There is no deliberate *keep this archive as a drive* any more, and nobody has
asked for one. If it is ever wanted it is an entry of its own and a card of its own.

A mount that is created and never navigated into is never pruned, because it has
never been stood in. That is bounded — one per archive somebody asked to open and
could not reach — and the alternative is the fault above, which cost a working
feature rather than a mount.

And `configForRoot()` widens the plugin interface by one virtual with a default, so
a backend that ignores it behaves exactly as before. What it commits us to is that
a mount which can be rebuilt says so in one place rather than each caller knowing
which schemes can.
