# ADR-0040: What leaves the window is a path on disk, and it leaves as a copy

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

Everything Mole can do with a file it does inside its own window. Handing one out
— to a browser's upload box, to another file manager, to a chat window — had no
route at all, and the way round it was to leave Mole, open something else and
find the file a second time in it.

Dragging a file out is one gesture with two decisions behind it, and both of them
can lose a file if they are taken carelessly. **What form the selection takes**
decides whether the receiver gets the file or a promise of it. **Which action the
drag offers** decides whether the receiver may delete the source afterwards —
because on every desktop Mole runs on, a move is something the *receiver*
performs, by taking the data and then trusting the source to remove it.

Mole's own argument is also in the way: a bucket, a NAS and an archive are the
same kind of drive as the disk. A row inside a zip has no path any other
application can open, so whatever "drag it out" means for that row, it cannot
mean handing over a path that exists.

## Decision

**A drag out carries `text/uri-list` of paths that already exist on disk, and it
offers `Qt::CopyAction` and nothing else.**

- One `QMimeData`, one `file://` url per row, in the order the rows were given. A
  directory goes as its own url rather than as its contents.
- **`Qt::CopyAction` alone.** Not one of several actions with copy as the default:
  the only action on offer. Nothing leaves Mole by being moved, however the
  receiving application asks.
- **Rows with no path on disk are left out of the payload, and said so.** Where
  that empties the selection nothing starts and the reason is reported; where it
  only thins it, the drag goes with what is left and the count that stayed behind
  is reported too. A drag is a gesture with no result to inspect, so silence
  there is indistinguishable from a broken pointer.
- **The step that hands the payload to the platform is a hook**, the way
  `FileLauncher::OpenHook` is. `src/ui` constructs no `QDrag`; the shell installs
  one that does, and the suite installs a recorder.
- Rows that are not on disk get a path of their own later, by being staged into a
  scratch directory first (MOLE-88). That is a second decision about *when* the
  bytes are fetched, and it does not change this one: what leaves the window is
  still a path that exists by the time it leaves.

## Reason

**Rejected: a lazy `QMimeData` that fetches on demand.** `QMimeData` can be
subclassed to supply its bytes from `retrieveData()` when the receiver first asks
for them, which sounds precisely like the answer for a 2 GB file on SFTP — no
copy unless somebody actually drops it. It is the wrong answer, and the reason is
which thread `retrieveData()` runs on: the UI thread, at the moment the receiver
asks, inside the drag's own event loop. The window would freeze for the length of
a network read, with no progress and no cancel, and the receiver would sit in its
own blocking call waiting for us. A read that takes four minutes is a hang, and
the user's only move is to kill Mole — which loses the drag *and* whatever else
was running.

**Rejected: `XdndDirectSave0`.** The X11 convention for "you tell me where to put
it and I will write it there" solves the same problem properly, and it is X11
only, unimplemented by most receivers, and dead on Wayland. A feature that works
on one display server for a minority of targets is a feature whose failure mode
is the interesting case.

**Rejected: offering move as well as copy.** It would make Mole's behaviour
depend on which modifier key the user happened to be holding when they released
the button over a window that may not even report what it did with the data. The
receiver's acknowledgement of a move is *advisory*: it says "I took it", and we
would delete on the strength of that. Copy-only means the worst case of a
misunderstood gesture is a duplicate file, which the user can see and delete.
That asymmetry — a duplicate versus a deletion — is the whole argument, and it is
the same one as
[ADR-0016](0016-a-copy-is-weighed-at-the-destination.md): the operation that
removes something is the one that has to be certain.

**Rejected: sending remote rows as their own `sftp://` or `s3://` urls.** Correct
in form and useless in practice, because the receiver would try to open a scheme
only Mole has a backend for. A url the receiver cannot resolve is worse than a
row that was honestly left behind: the drop appears to work.

**The hook, rather than a `QDrag` behind an `#ifdef`.** `QDrag::exec()` wants a
platform, a real window as its source and a pointer to follow, and it blocks
until the gesture ends. None of that exists in a test binary, and the platform
half of it exists on no CI runner. Everything that is Mole's own behaviour — what
the payload holds, which action is offered, which rows were left out — is
upstream of that call and testable without it, which is exactly where the seam
goes.

## Consequences

- `src/ui/DragSource` is the only place that builds a drag payload, and it links
  no more than `mole_ui` already did: `QMimeData` and `QUrl` are QtCore.
  `tests/ui` stays on `QCoreApplication`, headless, like every other binary
  there.
- A receiving application can never make Mole delete a file. It also can never
  make Mole *move* one, which will read as a missing feature to somebody arriving
  from a manager that offers it; the guide says so rather than leaving them to
  find out (MOLE-89).
- The real `QDrag` is reachable only through the running application, so what it
  does with the payload the hook is handed is checked by the walkthrough and by
  hand, not by a unit test. The seam is drawn so that the untested part is one
  call with no branches in it.
- Any later payload format — `application/x-kde-cutselection`, a
  `text/plain` fallback for a terminal — is an addition to one function, and one
  test asserts the whole payload rather than a format at a time.
