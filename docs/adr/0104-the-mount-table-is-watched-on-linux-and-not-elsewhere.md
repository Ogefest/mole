# ADR-0104: The mount table is watched on Linux, and not watched elsewhere

- **Date:** 2026-11-15
- **Status:** Accepted

## Context

Mole asked the operating system what is mounted once, on the startup path in
`AppController`, and never again. A USB stick plugged in afterwards did not exist
as far as Mole was concerned, and one pulled out was still offered. Fixing that is
the epic *A drive plugged in while Mole is running*. Its first piece, `MOLE-428`, is
how Mole learns that anything changed. The diff against the drive list, and what to
do with a drive that vanishes under an open pane, are later tickets.

Two things were already true and constrain the answer:

- On Linux, `SystemVolumes::enumerate()` reads `/proc/self/mounts` as text and
  touches none of the filesystems in it. That is deliberate: `MOLE-361` found that a
  `statvfs` on a stale NFS or SMB mount blocks for the kernel's timeout, which hung
  Mole at startup with no window.
- On macOS and Windows, the same function falls back to
  `QStorageInfo::mountedVolumes()`. That is a `statvfs` per mount, so one dead
  share blocks it.

## Decision

A class in core, `MountTableWatch`, sits beside `SystemVolumes` and emits
`changed()` when the mount table reads differently from the last time. It knows
nothing about drives.

- **On Linux it is a watch.** It holds a `QSocketNotifier` of type `Exception` on
  an open descriptor of `/proc/self/mounts`. The kernel raises `POLLPRI` on that
  descriptor when the table changes, and reading the file again through the same
  descriptor re-arms it. On each wake the watch re-reads the table and emits only
  if the text differs, so a mount and its unmount between two reads say nothing.
- **Elsewhere it does nothing.** On macOS, on Windows, and on a POSIX system with
  no `/proc/self/mounts`, it starts no notifier and no timer. `isWatching()` is
  false, `whyNotWatching()` says why, and the drive list behaves as it did before:
  read once, at startup.

## Reason

**Not a timer on Linux.** The cost of a watch while nothing happens is zero. This
was measured on 2026-09-05 in a private mount namespace: the descriptor woke
700 ms after a tmpfs was mounted, and after the re-read it stayed blocked with no
wake-ups. It was confirmed again through the class itself on 2026-11-15. A timer
of a few seconds would re-read the table forever to find it unchanged, and it would
still be up to a period late. A watch that woke spuriously would be a timer with
extra steps, so the test suite holds the opposite: while the table is unchanged,
nothing is read and nothing is sent.

**Not a timer on macOS or Windows either.** What `enumerate()` reads there is a
`statvfs` per mount. Polling it on a schedule would bring back `MOLE-361`, a stale
share blocking for the kernel's timeout, on every tick and on two platforms this
checkout cannot measure. The right answer there is a platform notification (Disk
Arbitration on macOS, device and volume notifications on Windows). Choosing and
measuring it needs those machines, the same wall *Windows and macOS, built and run*
exists for. Doing nothing keeps today's behaviour, which is a known limitation
rather than a new fault.

**Not udev, and not a D-Bus service such as UDisks.** They say a device appeared,
not that something was mounted. A stick that is plugged in but not yet mounted is
not a drive anybody can browse, and a network share mounted by hand has no device
at all. The mount table is the thing the drive list is made from, so it is the
thing to watch. It needs no new dependency and works the same in a desktop session
and without one.

**Not inotify on `/proc/self/mounts`.** Files under `/proc` do not report
modifications through inotify. `POLLPRI` is the interface the kernel documents
for this file (`proc(5)`).

## Consequences

- The watch is cheap enough to run for the life of the process, so the next
  ticket can listen to it without a setting.
- The class has a seam (`MountTableWatch::Source`): a descriptor, the condition
  it signals on, and a reader. Tests use the read end of a pipe and a fake table,
  so they need no mount namespace and no root.
- On macOS and Windows a drive plugged in while Mole is running still does not
  appear until a restart. That is now written down rather than accidental, and the
  class comment points here. Replacing it with a platform notification supersedes
  this record.
