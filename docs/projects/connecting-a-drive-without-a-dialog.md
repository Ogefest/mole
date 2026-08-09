# Project: connecting a drive without a dialog

- **Started:** 2026-08-09
- **Status:** planned, not begun
- **Tracked as:** the [Drives on the main screen](https://github.com/Ogefest/mole/milestones)
  milestone over issues on the
  [board](https://github.com/users/Ogefest/projects/1). This file holds the
  reasoning; the issues hold the work.

## Why

A configured S3 or WebDAV drive is invisible until it is mounted. To use one:
`F4`, File, Drives…, find it in a 240-pixel list, press ▶, close the dialog.
Four of those five steps are navigation, and the one that is not — pressing ▶ —
is in the last place anybody would look for it, because the dialog is called
*Drives* and is otherwise where you go to type a hostname.

Three things are wrong, and they turn out to be the same thing.

**The sidebar answers the wrong question.** It lists mounts. A drive configured
last week and not connected today is not in it at all, so the window offers no
evidence the drive exists. The one place in the application that is supposed to
say "here is what you can get at" is silent about most of it.

**Nothing on the main screen says whether a connection is alive.** Worse, the
dialog's own "connected" caption does not mean what it looks like it means:
`AppController::connectDrive()` returns as soon as the backend object is
*built*, and building one performs no I/O. A drive pointed at a host that has
been switched off reports itself connected, in green, until the first listing
fails. `checkDrive()` is the call that actually asks — and its answer is a
banner in a dialog the user is about to close.

**Credentials are the sharp edge.** `AppController::credentialsNeeded` exists,
is documented as "true when a configured drive cannot connect until the store is
opened", and **nothing in the interface reads it**. A drive whose password lives
in the locked store waits at startup in complete silence. There is no prompt, no
badge, and no failure — the drive simply is not there, and the way to find out
why is to open the Drives dialog and notice an amber band.

The dialog is doing two jobs: configuring drives and operating them. Being the
only place for the second is what makes the first feel heavy.

## What already exists to build on

More than it looks like, which is why this is a project of interface work rather
than plumbing.

- **`RemoteRegistry`** holds the configured drives, each with `mountAtStartup`,
  `secretFields` and `configFor()` — which already refuses to hand back a
  configuration when the store is locked, rather than connecting with a blank
  password.
- **`connectDrive` / `disconnectDrive` / `checkDrive`** are on `AppController`
  already, along with the `driveChecked(id, reachable, message)` signal.
- **`DriveCheckTask`** asks a real backend whether it answers, off the UI
  thread, and is covered by `tests/core/tst_DriveCheckTask.cpp`.
- **The join already exists.** `connectDrive()` sets `mount.id = drive.id`, so a
  mounted remote and the configuration it came from share a key. Nothing has to
  be invented to tell which sidebar row is which configured drive.
- **`QmlAppHarness`** drives the real window headlessly, so every one of these
  states can be asserted without a display.

## The decision this rests on

**Mounted is not reachable, and the interface must not conflate them.** A drive
has been *connected* when there is a backend object and a mount; it is
*reachable* when something has asked the far end and been answered. Those are
different facts with different costs — one is free and local, the other is a
network round trip — and a green dot that means the first while looking like it
means the second is worse than no dot at all, because it is believed.

**No liveness polling.** `QuerySpaceTask` already runs against every mount every
minute, and reusing it is the obvious idea and the wrong one: it deliberately
emits nothing when a backend cannot answer, because "unknown capacity" is a
normal outcome for a bucket. Silence from it cannot be read as unreachable. The
alternative, a dedicated poll, means steady network traffic against every
configured drive whether or not anybody is looking at the sidebar — for a file
manager that may have ten of them, on a laptop, that is a real cost for a dot.

So state changes at the three moments something is actually learned: when a
drive is connected, when a check is asked for, and when an operation against
that drive fails. Between those it shows what was last true, with the time it
was true. A stale answer honestly labelled beats a fresh answer nobody paid for.

Both of these go in **ADR-0017**, written alongside the first issue.

## The states a drive can be in

One row in the sidebar per drive, whether or not it is connected, in one of
these.

| State | Means | Row shows |
|---|---|---|
| `Local` | a disk or an archive: no connection lifecycle at all | what it shows today, unchanged |
| `Disconnected` | configured, not mounted | the name, muted, and a connect affordance |
| `Locked` | configured, needs a secret, and the store is not open | the name, muted, and an unlock affordance |
| `Connecting` | a connect or a check is in flight | the name and a spinner |
| `Connected` | mounted, and the last check answered | the name in full, capacity when it has one |
| `Unreachable` | mounted or not, and the last check failed | the name, the reason, and a retry |

`Local` is listed because the point of the VFS layer is that a local disk, a
share and an archive are the same kind of row. That does not change: they are
the same kind of row, and now some of those rows have a connection.

## Order of work

1. **The model.** The sidebar's list stops being mounts and becomes drives —
   mounts plus configured drives that are not mounted, joined on the id they
   already share. Headless, and the place every later issue gets its state from.
   ADR-0017 lands with it.
2. **The row.** Connect, eject and check from the sidebar, and the state made
   visible.
3. **Reachable, not just mounted.** The check runs on connect, the row shows
   what it found, and nothing claims green before an answer.
4. **Unlock where the problem shows.** `credentialsNeeded` finally read, a
   passphrase asked for once, in the window rather than in a dialog.
5. **Without the mouse.** Connecting a drive from the command palette, because
   this application is keyboard-first and a feature reachable only by pointer is
   half-built.
6. **The dialog goes back to one job.** Configuration. It keeps what only it can
   do and stops being the only way to do everything else.
7. **The guide.** Network drives shipped without a page in `docs/guide/`, and
   this is the change that makes one worth writing.

## Acceptance

With one S3 drive and one SFTP drive configured, both needing passwords, and the
credential store locked:

- the main window shows both drives, muted, marked as needing a passphrase,
  before anything is opened;
- one passphrase entry connects both, and the rows change state as it happens;
- a drive whose host is switched off ends in `Unreachable` with the reason on
  the row, and never passes through `Connected`;
- ejecting and reconnecting either drive never opens a dialog and never needs
  the mouse;
- every one of those states is asserted by `QmlAppHarness` against the real
  window, with no live server and no sleeps.
