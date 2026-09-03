# ADR-0093: One way to open a SQLite file, and what a wrong version does

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

Mole keeps three SQLite files, for three unrelated reasons. The **index** is the
one ADR-0065 and ADR-0066 are about. A **delimited file** is imported into a
scratch database so a hundred-million-row CSV can be paged, filtered and sorted
without being held in memory. A **`.sqlite` file the user is looking at** is read
in place by the preview.

Each of them opened SQLite its own way. Two named a connection by the calling
thread's address in hexadecimal and one cached per thread; two set
`journal_mode`, `synchronous` and `busy_timeout` and looked at none of the
results; `CountTableRowsTask.h` referred to "`connectionNameFor()` in ..." as
though there were one of it. Three copies of a thing nobody was maintaining as
one drifted into three separate faults, each invisible until it cost something:

- A pool thread that touched the index and then expired left its `QSqlDatabase`
  in Qt's registry for the rest of the session, with its page cache and its
  `-shm` mapping. The registry was keyed on `QThread*`, so the entry was only
  ever noticed when the address came round again — and then it was erased from
  the hash without `QSqlDatabase::removeDatabase()`, so `close()` could not see
  it either.
- Two previews of one `.sqlite` file on the drawing thread built the same
  connection name, because the name was the thread's address plus a hash of the
  path. Qt's `addDatabase` replaced the first and warned; both objects then read
  through the second's connection, and whichever closed first left the other's
  `rows()` failing for the rest of its life — a blank grid, with no error
  anywhere.
- `PRAGMA journal_mode=WAL` was executed and its answer thrown away. An index
  parked on a filesystem that will not have it — `MOLE_INDEX_PATH` pointing at a
  network mount — ran in rollback-journal mode instead. Readers queued behind
  writers again and the whole of ADR-0065 was undone, silently.

Separately, the index accepted a file written by a *newer* Mole without a word,
and then wrote to it. The credential store has refused one since it shipped.

## Decision

**One `sqlite::Connection` opens every SQLite file Mole touches.** It owns the
per-thread registry, names connections with a token unique to the object rather
than to the thread and the path, applies the pragma block, and removes a
connection when the thread that opened it ends.

**`journal_mode` is read back, and a file that was promised WAL and cannot have
it does not open.** The index and the delimited store both ask for it; the
preview, which is reading somebody else's database, asks for nothing.

**An index whose schema version is higher than this build understands is
refused, with a message saying so.** Not opened read-only, and not opened at all.

## Reason

The connection could have stayed three copies with the three faults fixed
separately. It was already three copies *because* nobody had decided it was one
thing; fixing them in place would have left the fourth drift to be found later,
and the comment in `CountTableRowsTask.h` pointing at a function that did not
exist is what that costs.

Naming by a token per object rather than per thread-and-path is the part that
had to change rather than be tidied. Two readers of one file are two readers:
an address-and-path name cannot say that, and the failure it produces —
one object silently using another's connection — is invisible until the other
one closes.

Removing on `QThread::finished` was chosen over sweeping the registry
periodically, or over a weak handle. The hook captures the connection's *name*
and nothing else, so it holds no pointer to the object that made it and cannot
outlive one; the context object is the thread itself, so Qt drops the connection
if the `QThread` goes first; and it is a direct connection, so the removal
happens on the thread that owns the database, which is where Qt requires it.

Refusing rather than warning about WAL is the harder call, because it turns a
slow index into one that will not open. It is the right way round: the promise
WAL makes is the one ADR-0065 built the locking on, and a promise that quietly
stopped holding is worse than a refusal, because nobody goes looking for it. The
message names the mode the filesystem answered with, so the answer — move the
index, or accept it elsewhere — is available without a debugger.

For a newer schema, refusing rather than opening read-only follows the
credential store, which is the only other file with a version in it. Read-only
was the alternative and was rejected on cost: it is a second mode that every
reader and every task would have to keep working for ever, for a case that only
arises when somebody has downgraded. There is nothing safe to write into columns
this build has never heard of, and an older Mole helpfully rewriting a newer
index is a worse outcome than being told to use the newer one.

## Consequences

A new SQLite user in this codebase gets the registry, the naming, the pragma
block and the thread cleanup for nothing, and does not get to invent a fourth
convention. The settings it does need are stated in one struct at the point the
file is opened, which is where a reader looks for them.

An index or a scratch store on a filesystem without WAL now fails to open where
it used to run slowly. That is a behaviour change a user can see, and it is
deliberate; `MOLE_INDEX_PATH` on a network mount is the case that will meet it.

A downgrade now says what is wrong instead of appearing to work. Nothing repairs
a newer index — the only way back is a newer Mole — and the message says which
schema it wants, which is what makes that actionable.

The per-block schema version this ADR sits beside (writing `PRAGMA user_version`
inside each migration's own transaction, and skipping an `ADD COLUMN` for a
column that is already there) means an interrupted upgrade is finished on the
next open rather than refused for ever. Migrations stay append-only, and a new
one that adds a column must go through the same guard to keep that true.
