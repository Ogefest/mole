# ADR-0089: A store says when it could not write, and keeps what it could not read

- **Date:** 2026-09-03
- **Status:** Accepted

## Context

Mole keeps eight things on disk as plain JSON: preferences, bookmarks, sets,
alert rules, schedules, chains, configured drives and the session. Each grew its
own store, and each store wrote out the same two sequences by hand — open,
parse, check a version; then create the directory, `QSaveFile`, write, commit —
with its own copy of the `MOLE_…_PATH` override in front of them. Thirteen copies
of the last of those.

The repetition was not the cost. The cost was that the same two faults were in
all of them at once.

**A write that did not land said nothing.** Every `save()` returned `bool` and
nearly every caller used it as a statement: twenty-seven of twenty-nine sites.
So a full disk, a read-only configuration directory or a `mkpath` that failed —
`Preferences` did not even test that one — left the model changed, the window
showing the change, and the file exactly as it was. The loss arrived at the next
start, silently, and there was no line anywhere saying why. The schedule is the
sharpest case: `ARCHITECTURE.md`'s "a job that quietly never runs is the one
failure nobody can diagnose" is about that file.

**A file that could not be parsed was replaced by an empty one.** Four stores
cleared their model, failed to parse, returned `false` and kept nothing — and the
next write put the empty model over the file. One stray byte in `drives.json`, a
hand edit with a trailing comma, and every configured drive was gone, its
passwords orphaned in the credential store under ids that no longer existed
anywhere. `SessionLog` had done the right thing with its own file for months: it
moves the bad one aside.

## Decision

**One base, `JsonFileStore`, and the file class under it.** `JsonFile` reads,
writes atomically and keeps what it cannot read; `JsonFileStore` is a `QObject`
holding one and adding two signals. Six stores derive from it. `BookmarkModel`
is a `QAbstractListModel` and cannot, so it holds a `JsonFile` and carries the
signal by hand; `SessionStore` is not a `QObject` at all and returns its reason
through an out-parameter.

**`save()` is `[[nodiscard]]` everywhere,** so the compiler names every caller
that drops the answer, and each mutator returns it.

**A failure is reported once, by the store, and not by each caller.** One
warning in the new `mole.store` category and one `saveFailed(reason)` that
`AppController` turns into a notification for every store in one place. The
reason names the file and nothing else — it goes on a screen and into the session
log, and neither may carry a directory off somebody's machine (ADR-0064).

**A file that cannot be parsed is moved to `<name>.broken-<timestamp>`,** and
the store carries on with an empty model. Writing is refused only while the
unreadable file is still there, which is what a directory nobody may write into
looks like.

**The session file keeps its old behaviour on the way in.** A session that
cannot be parsed still degrades to "start fresh", which `ARCHITECTURE.md`
decided and which is right for a file the user rebuilds by using the
application. A drive list, a set or a schedule is not that.

## Reason

**Why the change signal still fires when the write failed.** The ticket
suggested emitting `…Changed` only on success. The model *has* changed, and a
view that did not follow it would show something different from what the
application is actually using — a third state, worse than the two there were. The
alternative that would justify not emitting is rolling the model back, and that
cannot be done honestly: `RemoteRegistry::remove()` has already deleted the
drive's credentials by then, and a partial rollback is a worse lie than a full
notification. So: the model changes, the view follows, and the user is told the
disk did not take it.

**Why the store goes on working after the file has been moved aside.** Refusing
every write for the rest of the session, which is what "refuse to save until an
explicit reset" would mean, leaves a feature that appears broken with only a
toast to explain it — and once the old file is safe there is nothing left to
lose. The refusal is kept for the one case where the file could *not* be moved,
because there the old contents are still under the name being written.

**Why a timestamp in the kept name.** A second bad start must not overwrite the
copy the first one kept, which would be this exact fault one level up.

**Why not `QSettings`.** It has none of this — a failed write is unobservable
there too — and it would change the file format under every existing
installation to gain nothing that is being asked for here.

**Why the base is a class and not a set of free functions.** The two signals are
the point of it: the reporting has to reach the shell, and a free function has
nowhere to report to. `JsonFile` underneath *is* the free-function half, which is
why the two stores that cannot inherit can still use it.

## Consequences

Every configuration file in Mole now behaves the same way when the disk refuses:
the change stays on screen, a notification says what could not be written, and
the session log has the reason. Every one of them keeps a file it cannot parse.

`tests/core/tst_JsonFileStore.cpp` holds the shared behaviour once, rather than
the same case appearing in seven suites — which is the point of there being one
implementation. The stores whose *own* loss is distinctive keep their own case:
the drive list, because its file is the only record of which credentials belong
to what; the schedule, because a rule that stops running is invisible; and the
sets, which are the plainest "somebody built this by hand".

A ninth store added later gets all of it by deriving, and the compiler will make
its author read the answer to `save()`.
