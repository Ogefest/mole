# ADR-0079: A grid reads its rows on a task when its source says it may

- **Date:** 2026-08-23
- **Status:** Accepted

## Context

Three viewers stand on one interface. `ITableSource` is what makes a delimited
import, a SQLite table and a Parquet file all readable by the same grid, and
`TableModel` is the only thing that reads them. It reads from `data()`, which is
the thread that draws:

```cpp
QVariant TableModel::data(const QModelIndex& index, int role) const
{
    ...
    ensureLoaded(index.row());   // -> m_source->rows(offset, kChunkRows, filter)
```

The interface said, in its own comment, that an implementation "must be usable
from the interface thread, which in practice means answering a windowed query
quickly rather than scanning". [ADR-0045](0045-a-grid-shows-a-page-of-a-table-and-the-page-is-a-fixed-five-thousand-rows.md)
is what made that plausible: a page is a fixed five thousand rows, so the largest
offset any query can carry is bounded, and `LIMIT 500 OFFSET m` with `m` inside a
page is a cheap query for a store and for a database.

For Parquet it was never true, and MOLE-287 is where that was measured.
`ParquetTable::rows()` answered a window by reading the **row groups it touched**,
and a row group is whatever the writer chose — the convention is 128 MB or a
million rows, and plenty of tools write one group for the whole file. So scrolling
into a file like that read and decompressed the entire thing, into memory, on the
thread that draws, to show fifty rows of it; `CombineChunks` then copied it again.
Typing in the filter box was the same fault with a second multiplier: a filtered
read starts from row nought, and the count beside it walked the file again for
every four thousand rows it counted.

Two things therefore needed deciding, and only one of them is local. The reader's
own bound is a fix inside `ParquetTable` — read a batch at a time, stop where the
answer stops. Where the reading happens is a claim about the interface all three
viewers implement, and that is what this record is for.

## Decision

**A source declares whether it may be read away from the thread that owns it, and
the model reads it on a task when it does.**

`ITableSource` gains one question:

```cpp
virtual bool canBeReadOnATask() const { return false; }
```

A source answering true promises two things. That `rows()`, a filtered
`matchingRows()` and `columnWidths()` may be called from a pool thread — never two
at once. And that `headers()`, `totalRows()` and an *unfiltered* `matchingRows()`
answer without touching the file, because those are what the model needs to know
the shape of a table before it has any of it, and it goes on asking them where it
always did.

For such a source, `TableModel`:

- asks for a chunk instead of fetching it. `data()` answers with a blank for a row
  it has not got yet, the way it already did for a window that failed to read, and
  a `dataChanged` over that chunk's rows is what puts them on screen when they
  land.
- keeps **one read outstanding** and queues the rest, because the promise above is
  one question at a time.
- takes a filtered count on a task too, holding `matchingRows` at -1 until it
  lands — which this model has always used for "not counted yet", so the footer
  shows a blank rather than a number that is wrong.
- **abandons what is outstanding** whenever what the view is showing changes: a
  page move, a filter, a new file. Those answers are about a page, or a filter,
  that has been left.
- **shares ownership of the source.** `setSource()` takes a `std::shared_ptr`, and
  a read holds its own share for as long as it runs.

`ParquetTable` answers true. The delimited store and the SQLite table answer
false and are read inline, exactly as before.

## Reason

The alternatives were named in the ticket and both were considered.

**Every source behind a worker, for every kind of table.** This is the tidier
sentence and the worse change. Both sources that answer false hold a SQLite
connection belonging to the thread that opened it — `SqliteTable` hashes the
calling thread into the connection name for exactly that reason — so moving their
reads means every source growing per-thread connections, and the two most recent
faults in this area (MOLE-289's lock-out, MOLE-290's use-after-free) were both
about SQLite and threads. It would be a large change to code that is not the fault,
in exchange for nothing measurable: a windowed query with an offset inside a page
is already cheap, which is what ADR-0045 bought.

**A local fix in the Parquet reader alone.** Bounding the reader is necessary and
it is done, but it does not settle the question. Even a batch is a decode, a
filtered read of a file with no index has to walk it, and the walk is bounded at
200,000 rows rather than at "quick". Any of that on the thread that draws is the
window not answering, and the house rule — the interface thread does not touch
storage — is not satisfied by making the touch smaller.

**A declaration rather than a blanket rule**, because the interface could not
enforce a blanket rule and would only have been describing a hope. What it can do
is make each implementation state what it is, and put the awkward case where
somebody reading the code will find it: a source that is read off the thread that
owns it says so in one line, next to the reads.

## Consequences

- A Parquet file's rows arrive a moment after its columns do, and the viewer says
  so: `TableModel::reading` is true while something is outstanding, and the strip
  shows it. A grid filling in must not read as a grid with holes.
- `cellAt()` and `blockAsText()` — the copy path — answer from what has been
  fetched. Copying a block of rows the view has drawn is unaffected; a selection
  reaching rows that have not arrived yet copies blanks for them. Accepted: the
  alternative is a read on the thread that draws, which is the fault.
- A test can hold the rule. `ReadTableTask::ranOn()` names the thread the read
  ran on, and `ParquetTable::rowsDecoded()` counts what Arrow decoded — so both
  halves are asserted on what happened rather than on how long it took.
- Nothing changed for the two sources that answer false, which is what keeps this
  change small enough to be sure about. Moving them is a separate decision, and it
  starts with a filter typed into a SQLite grid — still a `LIKE` scan on the thread
  that draws, and still bounded by nothing.
