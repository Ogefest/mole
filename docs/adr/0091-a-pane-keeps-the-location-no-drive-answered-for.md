# ADR-0091: A pane keeps the location no drive answered for, and asks for a drive rather than connecting one

- **Date:** 2026-09-03
- **Status:** Accepted

## Context

`BrowserPaneController::load()` resolved the uri against the mount table first
and, when nothing answered, showed "No drive is mounted for …" and returned
before `m_current`, the history and `locationChanged()` were touched. A pane in
that state did not know where it was pointed, and that had three consequences
that all read as different bugs:

- A tab restored before its drive was connected — a drive whose password is in
  the credential store, or one not marked "connect at startup" — came up on the
  start folder. `BrowserController::saveState()` writes `activePane()->currentUri()`,
  so the next debounced save wrote the start folder over the remembered one and
  the folder was gone for good. Connecting the drive afterwards brought nothing
  back, because nothing was waiting for it.
- `refresh()` is guarded by `if (m_current.isValid())`, so Refresh on such a pane
  did nothing at all. There was no way to retry by hand either.
- The rebuild of a disposable mount lived in `navigateTo()`, whose comment claimed
  "every way into a place goes through here". `goBack()`, `goForward()`, `goUp()`
  and `refresh()` call `load()` directly, so leaving an archive — which prunes the
  mount as the last reader goes — and stepping back into it failed to resolve.
  ADR-0083 lists a back step as one of the three things the rebuild exists for.

Somebody has to connect the drive, and a pane is the wrong object to do it: the
configured drives, the credential store and the passphrase prompt all belong to
`AppController`, which is where `goTo()` already arranges them for every other
way into a place.

## Decision

A location a pane cannot list is still the pane's location. `load()` records
`m_current`, appends the history entry and emits `locationChanged()` **before**
anything is asked of a drive, and the failure to resolve only sets the error text
and clears the listing. The rebuild of a disposable mount moves from
`navigateTo()` into `load()`, so every door gets it.

When no drive answers, the pane says so on the EventBus — `postDriveNeeded(uri)`
— and remembers the uri. `AppController` answers that event with the same
`prepareDriveFor()` readiness step `goTo()` uses: it connects the configured
drive, or asks for the passphrase and connects it once the store opens. The pane
retries on its own when the mount table changes and a drive now answers for where
it is pointed.

## Reason

**Why the pane keeps the location.** It is the only object that knows where the
user meant to be, and three separate features read it: the path bar, Refresh and
the session file. Any fix that left `m_current` empty had to add a fourth place
to remember it — and `saveState()` would still have had to be taught the
difference between "no location" and "a location that did not load", which is
exactly the distinction `m_current` already draws.

**Why an event and not a call.** The pane holds `PluginServices` — the vfs, the
tasks, the index, the bus — and none of the drive machinery. Handing it a
`RemoteRegistry` and a way to raise the passphrase dialog would give every pane,
including a plugin's, the ability to connect drives and prompt the user, to fix a
case that is about one pane in one tab. The bus already carries questions of this
shape and this is what it is for: the pane says what it needs and does not learn
who arranges it.

**Why the pane is not navigated afterwards.** `goTo()` navigates the current tab,
which is right when a person clicked something. It is wrong here: the tab that
wants the drive may be in the background, and dragging the foreground tab to a
restored tab's folder because a passphrase was entered would be baffling. So
`arrangeDriveFor()` deliberately does not navigate — the mount appearing is the
answer, and every pane waiting on that drive picks it up, which also handles two
panes on one drive without counting them.

**Why the retry watches `VfsManager` and not the bus.** Mounts are added and
removed on the thread that owns the mount table, which is the thread the panes
live on, so the pane can be listing the drive in the same turn the sidebar starts
showing it. Going through the bus would have been a queued hop for no gain. The
retry asks the mount table whether a drive answers before re-listing, so a pane
waiting on a drive nobody is connecting does not re-list on every unrelated mount.

**What was rejected.** Rolling the history index back on a failed resolve, which
is what the task asked for: it leaves the pane at a place the history index does
not point at, so Back is disabled while the previous folder sits right there in
the list, and it stays disabled after a successful Refresh. A browser leaves an
unreachable page as the current history entry with Back working, the pane's own
history is already described as working "the way a browser does", and that is
what landed. Also rejected: polling for the mount, and having `AppController`
walk the open tabs looking for panes with an error — both are the same fact
arriving late and by inspection rather than by announcement.

## Consequences

The history can hold a location that never listed. That is deliberate and it is
what makes Back work from a failed step, but anything reading the history as a
record of folders that were shown is wrong to.

`EventBus` gains `driveNeeded`, and it is a question rather than a statement —
the first event of that shape. Nothing is guaranteed to answer it: with no
`AppController` listening, as in a unit test, a pane simply stays where it is
with its error text, which is the old behaviour minus the forgetting.

Entering a passphrase at startup now brings back every tab that was restored onto
a drive behind the store, not only the drives marked "connect at startup". A
restored tab can therefore raise the passphrase prompt, which it could not
before; it is the same prompt `goTo()` raises and it is asked once however many
tabs are waiting.
