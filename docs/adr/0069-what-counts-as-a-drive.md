# ADR-0069: What counts as a drive

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

`SystemVolumes::isInteresting()` decided what the sidebar's drive section shows.
It admitted a mount for one of three reasons: its root path is `/`, its type is a
network filesystem, or its root path begins with `/media/`, `/run/media/` or
`/mnt/`.

The reasoning for an allowlist was sound and is not in question. On a ZFS or
Btrfs machine `/var/lib/dpkg`, `/var/games` and `/boot/grub` are separate
filesystems — real mounts, utterly uninteresting, and no blocklist keeps up with
a fresh one per installed package.

What was wrong is that three mount prefixes are one Linux convention out of
several, and that the rule asks *where is this mounted* when the question is *is
this somewhere I keep files*. Run against a real Ubuntu-on-ZFS machine it
answered:

- a separate `/home` dataset — a real disk holding everything the user owns — is
  **not** a drive, because no prefix names it;
- a ramdisk at `/mnt/ramdisk` **is** a drive, because of where it happens to be;
- `/storage`, `/data` and `/pool`, where a great many people mount a second disk,
  are not drives, because they are named by no convention on the list.

The `/home` case was hidden because `AppController` mounts Home separately by
hand, so the folder stayed reachable while the volume was never listed.

## Decision

The allowlist stays, as one signal rather than the only gate. A mount is a drive
when any of these holds, in this order:

1. it is `/`, or a network filesystem, wherever it is mounted;
2. it carries the user's home directory, whatever it is called;
3. it is under one of the conventional media prefixes for the platform;
4. it is on a **real backing device** — a node under `/dev/` — and is **not
   under a system directory**.

And it is never a drive when its type is pseudo, when it is memory-backed
(`tmpfs`, `ramfs`, `devtmpfs`) wherever it is mounted, or when its device is a
loopback.

"A system directory" is a fixed, shallow list: `/boot`, `/etc`, `/usr`, `/var`,
`/opt`, `/srv`, `/run`, `/tmp`, `/snap`.

The home path is a parameter, like the platform, so the rule can be asked about a
machine that is not this one.

## Reason

**Rule 4 is what rescues `/storage` and `/data` without naming them**, and both
halves of it are load-bearing. Without the device test the whole ZFS dataset pile
arrives, because a dataset name like `pool/ROOT/ubuntu/var/lib/dpkg` is not a
device node — that single distinction keeps the pile out without naming any of
it. Without the system-directory test, `/boot/efi` arrives, because it sits on a
real partition.

**The system-directory list is not the blocklist the old comment rejects.** That
argument was against a list that would have to keep up with a separate dataset
per installed package. This is nine directories the Filesystem Hierarchy Standard
has named for thirty years, and it is consulted only after a mount has already
been found to be on a real disk.

**An ancestor-mount test was considered and rejected.** The ticket proposed
admitting "a mount on a real backing device that no ancestor mount already
represents". Read as "same device as an ancestor" it drops real volumes: a Btrfs
machine with `@home` and `@data` subvolumes on one device would lose `/data`.
Read as "an ancestor is already listed" it rescues nothing, since `/` is an
ancestor of everything. The system-directory test does the job it was reaching
for, and `enumerate()` already drops a repeated root path.

**The ZFS keystore at `/run/keystore/rpool` stays out**, though the ticket listed
today's "no" among its complaints. It is a real device-mapper node, so rule 4
would admit it but for `/run`. The complaint was about the *reason* — that the
answer came from where the thing happened to be mounted — and by the standard the
ticket itself sets, an encryption keystore is not somewhere anybody keeps files.
Excluding it deliberately is a different thing from excluding it by accident.

**Guessing at every convention is explicitly not the goal.** Rule 4 is the
general answer, and the escape hatch for whatever it still misses is a root added
by hand — `MOLE_DRIVES` proves the shape today.

## Consequences

On the machine this was found on, the list goes from three rows with a ramdisk
in it and no home volume, to four: the home dataset, the root, an ext4 disk under
`/mnt` and an NFS export. Twenty-six snap loopbacks, the dataset pile, the EFI
partition and the ramdisk are all out.

`SystemVolume::isRoot` marks the system's own volume, and it now does so on
Windows too, where nothing is `/` and the old test therefore marked no volume at
all and left the order to the names. It is deliberately *not* "the volume
carrying home" in general: on a machine with a separate `/home` dataset that
would put the home volume above the root and reorder a sidebar nobody asked to
have reordered. The struct's comment claimed the home meaning and the code never
had it; the comment is what was wrong.

The volume carrying home is now in the list as well as being mounted by hand, so
`AppController` skips it to keep one row per place, under the name people look
for rather than the name the dataset happens to have.

`MOLE_DRIVES` is still the only way to add a root by hand and is still not
reachable from the settings. That is a separate piece of work; nothing here
depends on it.
