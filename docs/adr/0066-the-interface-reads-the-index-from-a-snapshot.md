# ADR-0066: The interface reads the index from a snapshot

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

[ADR-0065](0065-the-index-serialises-writers-and-lets-readers-through.md) stopped a
read of the index queueing behind a scan. What it deliberately left is that the read
still happens on the thread that draws the window, and MOLE-264 names seven call sites
that do it. Looking at them closely, the count understates the shape of the problem in
two ways.

**Some of them are property getters, so they are not seven calls but seven calls per
binding evaluation.** `LiveSearchController::factKeys()` runs `volumesInsideRoot()` and
`coveringVolume()` — a `volumes()` query each — and then one `factKeys()` query per
volume in scope. `coverageNote()` calls `factKeys()` again. `indexCoversRoot()` and
`indexNote()` each call `coveringVolume()`. These are `Q_PROPERTY` reads that QML
evaluates whenever anything they depend on changes, so *a task per call is not
available to them*: a binding has to answer now, with something.

**One of them is on a hot path.** `BrowserController::refreshFolderFacts()` is connected
to `locationChanged` and to the alert store's `rulesChanged`, so it runs on every folder
change and every alert edit, not once at startup.

There is also an **eighth** site, which did not exist when the ticket was written:
`AppController::recordStartup()` lists the indexes in the session log
([ADR-0064](0064-what-a-session-log-says-when-nobody-asked.md)). The comment six lines
above it says drive space is left out because asking storage there would put a
synchronous read on the startup path, *and cites this very fault* — and then the next
block asks the index synchronously. Which is the argument for a mechanism rather than
eight fixes: a list of call sites written down in a ticket goes out of date, and it went
out of date during the session that fixed it.

## Decision

**The interface never reads the index. It reads a snapshot of it, kept up to date from
the task layer.**

A new host service, `IndexSummary`, holds what the interface asks for: the volume list,
and the fact keys per volume. It is owned by the shell, lives on the thread that draws
the window, and answers from memory.

- It refreshes by submitting a task, so every `IndexDatabase` query it makes runs on a
  pool thread. The task is background work and one of many, so a refresh does not
  scroll a user's real copies off the task strip.
- It refreshes when `EventBus::indexUpdated` arrives, which is already posted by every
  finished scan and by removing a volume — so the invalidation channel is one that
  exists and is already trusted, not a new one.
- It answers **three** states and not two: known, and here is what is in the index;
  known, and nothing is; **not asked yet**. The third is what keeps a late answer from
  reading as *this folder is not indexed* — a wrong answer that would be worse than the
  stall it replaces.
- It is appended to `PluginServices`, which is allowed to grow, so a plugin asking what
  is indexed gets the snapshot rather than the database.

**And the rule is enforced rather than remembered.** `IndexDatabase::doNotReadFrom()`
names the thread that draws the window, and a read arriving from it logs a warning that
names the caller. A ninth site added next year fails a test nobody had to remember to
extend.

## Reason

**Why a snapshot and not a task per call site.** MOLE-264 offered both and said the
choice is per site. It is a snapshot for all of them because five of the eight are
property getters, and a binding cannot await anything — it returns a value or it returns
the wrong one. Giving those a cache and the other three a task would mean two mechanisms
and two ways to be stale, for no gain: the answer is the same small table either way.

**Why the three-state answer.** With two states, the interval before the first refresh
lands says *nothing is indexed* — in the browser's folder facts, in the search form's
`indexCoversRoot`, and in what the coverage note tells the user their search will
cover. That is the failure mode this decision is most exposed to, and it is worse than
the freeze: a freeze is visibly the application's fault, while "not indexed" is a
confident false statement that sends somebody to re-scan a tree that is already there.
So *not asked yet* is a value the callers can see, and each one decides what to render
for it — the browser shows no tag at all, and the search form does not claim coverage
either way.

**Why the invalidation is `indexUpdated` and not a signal from `IndexDatabase`.**
`IndexDatabase` is not a `QObject` and making it one would put the interface's refresh
policy inside the storage layer. `EventBus::indexUpdated` is already posted from all
four places a scan can finish and from removing a volume, and `SearchFeatures` and
`IndexesFeature` already listen to it to re-read. This replaces those re-reads rather
than adding a channel beside them.

**Why not a `QReadWriteLock`, or a wider WAL change, or nothing at all.** Those were the
alternatives to ADR-0065 and were settled there. What is left after it is not a lock
question: it is I/O on the drawing thread, whose duration is decided by whatever else is
touching the database — a checkpoint, another reader, a cold page cache. `volumes()` is
quick on the machine it was written on. That is the whole reason this is worth doing and
not worth measuring first.

**Why a guard and not a longer list in a test.** The ticket asked for the sites to be
named in a test so that an eighth is a line somebody has to add on purpose. A guard is
strictly stronger and needs nobody's attention: the eighth site above proves the weaker
version does not hold, because it was added, by the same author, in the session that
fixed the other seven.

## Consequences

- **The interface's answer can be one refresh out of date.** It is bounded by a task
  round trip and only after something has changed the index, which is a scan finishing
  or a volume being removed — both of which post the event that starts the refresh. The
  index's own answer was already older than that: it describes the last finished scan.
- **A fact key can be offered for a volume that no longer has it** until the refresh
  lands, which offers a search field that returns nothing. The reverse — a field missing
  for a moment — is the same size of wrong and neither is worth a synchronous query.
- **`IndexSummary` reads every volume's fact keys on each refresh**, which is one query
  per volume rather than one per volume per binding evaluation. On the reproduction
  index that is a handful of queries a scan, against thousands a minute today.
- **Writes from the drawing thread are the same fault and are not fixed here.**
  `IndexesController::removeVolume()` calls straight into the database, and writers are
  still serialised, so removing an index can wait for a scan's transaction. It is a
  separate ticket, and the guard covers reads only so that the suite stays green in the
  meantime; when that lands, the guard covers writes too.
- `open()`, `close()` and `applyMigrations()` are outside the guard by construction:
  they run on the thread that starts and stops the application, which is the same
  thread, and they are the two moments when that is correct.
