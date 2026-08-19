# ADR-0052: A drive's dot says what it is doing, and says it the same way for every drive

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

Every drive row in the sidebar carries an eight-pixel dot, and it meant two
different things depending on which row it was on.

`DriveListModel::stateOf()` gave a mount with no configured drive behind it —
a local disk, an open archive, the scratch space — `State::Local`, because *"there
is no connecting or disconnecting to be done to it"*. `stateSeverity()` had no case
for `Local`, so it fell through to the shared `idle`, which the sidebar paints in
`mutedText`. That is the same grey `Disconnected` and `Locked` wear, and there it
means something real: *configured, not connected, and could be*. A local disk is
not doing anything, and it is not waiting to be connected either.

`docs/guide/drives.md` taught that meaning — *"grey like the ones nobody has
connected"* — and one line later promised *"a dot for what the drive **is
doing**"*. It was not one.

The obvious remedy is to take the dot away from the rows it says nothing about,
and that is wrong for a reason worth writing down: `Sidebar.qml` already gives
absence a meaning. *"The drive rows use these; the bookmark rows leave them
alone"* — a bookmark row has `severity: ""` and therefore no dot at all. An empty
slot on an idle drive would make two different kinds of row look identical, which
is this fault inverted, and it leaves a hole in a column of dots that reads as
something missing rather than as something calm.

## Decision

**The dot says what a drive is doing, and it is the same question for every kind of
drive.** Whether anybody has a drive open, and whether work is running on it, are
knowable without asking the drive anything — and they are as true of a local disk
as of an S3 bucket.

Six states, highest first: **Unreachable → Busy → Open → Connecting → Not
connected → Idle.** A drive that is open *and* has work running on it reads busy,
because that is the more specific statement; a drive nothing can reach reads
unreachable whatever is being attempted on it.

| | Means | Applies to |
|---|---|---|
| **Unreachable** | mounted, and a check said the far end is not there | configured drives |
| **Busy** | work the user asked for is running on it | everything |
| **Open** | some tab's current location is on this drive | everything |
| **Connecting** | being connected right now | configured drives |
| **Not connected** | configured, not connected, and could be — including `Locked` | configured drives |
| **Idle** | available, and nothing is using it | everything |

**`State::Local` is not a state, and `State::Connected` is not either.** A local
disk is `Idle`, `Open` or `Busy` like anything else; what made it special — that it
cannot be connected or disconnected — is expressed by it never being able to reach
the first five rows rather than by a state of its own. A mounted drive nobody is
using is `Idle`.

**The appearance is spread over four channels, and each carries exactly one idea.**

| State | Dot | Colour |
|---|---|---|
| Unreachable | filled | red `#e5534b` |
| Not connected | **ring, hollow** | muted `#8b93a7` |
| Locked | ring, hollow | muted `#8b93a7` |
| Connecting | ring, hollow, **pulsing** | muted `#8b93a7` |
| **Idle** | **filled** | muted `#8b93a7` |
| Open | filled | accent `#4c9aff` |
| Busy | filled, **pulsing** | accent `#4c9aff` |

- **Hollow against filled** carries *not here yet* against *here*. That is the pair
  the old grey was conflating, and shape separates it at eight pixels where a shade
  of grey cannot.
- **Hue** is the kind: muted for nothing of yours, accent for yours and in use, red
  for broken.
- **Motion** is *happening right now*, and only that. The two transient states
  pulse; the hue says which.
- **Absence** stays what it already was: this row is not a drive.

**Green goes.** Under this model *connected* is `Idle` — available and unused.

**Nothing polls, and no drive is contacted to work any of this out.** `Open` is
learnt from a signal the application already sends itself: a tab's controller
answers `FeatureController::openLocations()`, the shell asks again whenever
`stateChanged()` says something moved, and `DriveListModel::noteOpenLocations()`
turns those locations into drives through the mount table. A location counts
whether or not its tab is the visible one, and two panes on one drive is still one
`Open`.

## Reason

**Why not simply drop the dot from the rows where it said nothing.** Because
absence is already spoken for — see the context above. Two kinds of row that look
identical is the fault this record exists to fix.

**Why shape rather than another colour for *not connected*.** Six states will not
fit in one channel, and adding a fifth grey is not a distinction anybody can make
at eight pixels. Shape is legible at that size and needs no legend: an empty ring
is plainly *not filled in yet*.

**Why the accent for Open rather than a new colour.** `Material.accent` is
`#4c9aff`, appears twenty-three times in the interface, and everywhere already
means *this is the thing you are on*. "Open in a pane" is that meaning exactly, so
a new colour would be a second vocabulary for one idea.

**Why green had to go, beyond it no longer having a state.** Green against red is
the one pair deuteranopia cannot separate, and it was carrying *connected* against
*unreachable* — the two states where being wrong matters most. Accent blue against
red is safe, and with hollow-versus-filled and motion beside it, colour is never
the only thing saying anything.

What replaces the moment of "it worked": the row's word changes, the hollow ring
becomes a solid dot, and the capacity bar appears, because a connected drive can
report its size. A brief green *flash* on a successful connection would be a
transient rather than a state and is one line — deliberately left out here rather
than assumed, and worth asking about.

**Why this is more compliant with ADR-0018, not less.** That record's argument was
that state changes when something is *learned*, and that nothing may poll:
reachability needs a check to be learnt, so it is only ever told to the model.
`Open` is learnt from a navigation signal the application raises anyway. Nothing in
ADR-0018 is reversed — its decisions about reachability and polling stand
unchanged, and this adds two states it does not touch.

**Alternatives considered.** A tooltip only, with no dot at all: the tooltip
already says the word, and a state nobody can see without hovering is a state
nobody reads. A count badge of open tabs per drive: more precise and less legible,
and the question people ask is *which drive*, not *how many tabs*. Colour alone
across six states: unreadable at this size and unreachable for anybody who cannot
separate two of the hues.

## Consequences

- **The guide's legend changed**, and `docs/guide/drives.md` teaches the new one in
  one short table. The old sentence about grey meaning *nobody has connected it* is
  gone, because it is now only half true.
- **Every drive keeps a dot**, and `Idle` is the dot that was there before — what
  changed is that it now states something true, because the meaning it used to
  share has moved to a different shape.
- **`Busy` comes from the tasks the user asked for**, landed the same day as the
  rest of this (MOLE-162). A task declares the locations it reads or writes through
  `Task::noteTouching()` in its constructor, and the model derives the set of busy
  drives when a task is appended or changes state. Two guards keep it from becoming
  noise, and both are asserted: `Task::isBackground()` excludes the application's
  own housekeeping — `QuerySpaceTask` runs per mount every minute and would
  otherwise pulse every row once a minute for ever — and a task that declares
  nothing lights nothing, which is the right default for a metadata read or a
  thumbnail decode. A copy declares both ends, so a transfer between two drives
  pulses both.
- **`FeatureController` gains `openLocations()`**, which is a new extension point,
  small and answerable with nothing: a feature that is not anywhere — a report, a
  bulk rename — inherits an empty answer and costs nothing. A plugin's tab
  participates in the sidebar's dot without being taught what a sidebar is.
- **A tab that moves now costs a walk of the tab list.** It is a handful of string
  comparisons against a mount table with a few entries in it, on a signal that
  already fires on every navigation to mark the session dirty.
- **The three-way severity vocabulary is smaller than the four-way one it
  replaces**: `idle`, `using`, `broken`. `attention` (amber) had one user,
  `Connecting`, which now pulses instead — motion says *happening* better than a
  colour that also means *nearly full* on the capacity bar two pixels away.
