# ADR-0021: The working name is not only for servers

- **Date:** 2026-08-10
- **Status:** Accepted. Extends [ADR-0020](0020-an-upload-in-progress-wears-a-different-name.md),
  which stands.

## Context

ADR-0020 gave remote uploads a working name — `<name>.mole-partial`, renamed into
place once every byte has arrived — because a process killed outright does not
get to delete what it wrote. It said, in as many words, that this applies to
SFTP, FTP and WebDAV.

The local disk has the same problem and one more.

`LocalFileSystem::openWrite()` opened the destination with `Truncate`. So a copy
onto an existing file destroyed that file's contents *before the first byte of
the replacement arrived*, and a process killed in between left neither: not the
file being replaced, and not the file replacing it. A remote upload interrupted
half way at least leaves the previous version alone, because nothing on the
server is touched until the write begins. The local one had already thrown it
away.

This is not the exotic case. It is a copy over a file of the same name, which is
what a re-run of a failed copy *is*.

## Decision

**One convention, local and remote alike.** The suffix, the rename, and the rule
about what happens if the destination is occupied move out of the network plugin
and into `core/vfs/PartialWrite.h`, where every backend uses them.

A user who loses power mid-copy sees `report.pdf.mole-partial` whether the copy
was to a NAS or to the disk in front of them, and it means the same thing in both
places. A convention that held in one half of the product would be worse than
none: it would teach people a rule that is sometimes true.

**And an overwrite is still an overwrite.** The check ADR-0020 introduced — the
destination must be free, or the result is not put in place — turns out to have
been quietly relying on the callers it happened to have. Sync's *overwrite* mode
writes onto a file that is meant to be there, and refusing that is not caution,
it is a bug.

So the question is asked at the right moment: **whether the destination existed
when the write began.** Existing then means the caller is replacing a file it
knew about, which is ordinary and proceeds. Appearing since means something
arrived during the write, which is data this write was never asked to touch, and
it is refused. Only the caller can tell those apart, and only because it looked
before it started.

## Reason

**Leave the local case alone; `Truncate` is what everyone does.** Rejected on
the evidence. It is what everyone does and it loses the file being overwritten,
which is the file most likely to matter — a re-run of an interrupted copy aims
at exactly the name whose previous contents are the only surviving copy.

**Keep two conventions, one for the network plugin and one for core.** Rejected:
the suffix is something a *user* sees in a listing, not an implementation detail
of a backend, and a name that means one thing on a drive and nothing on another
is a name that teaches nothing.

**Always replace whatever is at the destination.** Rejected — that is the
behaviour ADR-0020 exists to prevent, and it would have made this ADR a
retraction rather than an extension.

**Ask the caller for its intent through `openWrite`.** It would be more explicit
than probing, and it would change `IFileSystem` for every backend and every
caller, to say something the backend can find out for itself in one call. Worth
revisiting if the probe ever shows up in a profile.

## Consequences

- Every write now costs one extra look at the destination before it starts, and a
  rename after it finishes. On a directory of ten thousand small files to a
  remote drive that is ten thousand extra round trips, and it is the first place
  to look if bulk writes get slower.
- A local write is no longer in-place. The destination is a **new inode**: hard
  links to the old file no longer see the new contents, and permissions or
  ownership set on the destination beforehand are not carried over. For a file
  manager copying files this is right — the result should look like the file that
  was copied. For anything that edits a file in place it would not be, and
  nothing here does.
- `.mole-partial` files can now appear on local disks too. As on a server, they
  are visible, they mean exactly one thing, and removing them is a manual job —
  see [TODO.md](../../TODO.md).
