# ADR-0045: A grid shows a page of a table, and the page is a fixed five thousand rows

Date: 2026-08-18

Status: accepted

## Context

The tabular viewers — delimited text, SQLite and Parquet — share one grid and one
model, and the model offered every row of a table at once. `rowCount()` returned
the whole matching count, so a table of ten million rows was a view of ten
million rows and the scrollbar was an offset into the file.

The fetching was already windowed and always had been: five hundred rows at a
time, twenty-four of those windows cached, so the model never held more than
twelve thousand rows however far anybody scrolled. What was unbounded was the
*offset* it fetched at. `SELECT … LIMIT 500 OFFSET 9000000` is answered by
stepping over nine million rows to reach the ones that were asked for, and the
interface thread waits for it. One drag of the scrollbar issues a run of those,
each one costing more than the last.

That is the same fault as the counting that MOLE-186 moved off the interface
thread, in a different place: an operation whose cost grows with the size of the
data, paid on the thread that draws.

## Decision

**The grid shows one page of a table at a time, and a page is five thousand
rows.** `TableModel` owns the page: `rowCount()` never exceeds five thousand,
row indices in the model are relative to the page, and the fetch window lives
inside it — so scrolling within a page still costs one query per screen, and the
largest offset any query can carry is the page's start plus five thousand.

The controls to move between pages are under the grid, in `DataGrid.qml`, so all
three viewers get them together: first, previous, next and last, the page
number, and which rows are on screen — *rows 5,001–10,000 of 1,284,003*. The
strip is hidden when there is only one page, so a small file looks exactly as it
did. `Ctrl+PgDn` and `Ctrl+PgUp` move between pages and `Ctrl+Home` and
`Ctrl+End` jump to the ends, which is what `PdfPreview`, `TextPreview` and
`HexPreview` already do and what `ViewerKeys.qml` states as the convention.

**Five thousand is a constant.** It is not a preference and not adaptive.

## Reason

**Why a page rather than a faster offset.** There is no faster offset. `OFFSET n`
in SQLite is defined as discarding *n* rows, and a Parquet reader has to walk row
groups to find the one that holds row nine million. Keeping the whole table
behind one scrollbar means promising a seek the sources cannot perform.

**Why not a virtual scrollbar over the whole table.** Keeping the appearance of
one long table while quietly refusing to answer the middle of it is worse than a
page: the reader is told they can go somewhere they cannot, and the grid stalls
rather than saying so. A page is a claim the viewer can keep.

**Why five thousand.** It is far more than a screen, so paging is something you
reach rather than something you fight: a full-height grid shows about forty rows,
so a page is a hundred screens. It is small enough that the largest offset in any
query is five thousand — a walk no source notices — and that a page of the widest
plausible row is a few megabytes if it were ever held at once, which it is not.

**Why not a setting.** Nobody has asked for one. A setting would need a
paragraph in the guide explaining what it trades, and the trade is between two
costs the reader cannot see: the offset the source has to step over and the rows
the view has to lay out. A number chosen from that paragraph would be a guess
made by somebody with less information than the code has.

**Why in the model rather than in each viewer.** The three viewers already share
`TableModel` behind `ITableSource`, which is what the interface was made
source-agnostic for. Paging in the SQLite viewer alone would have left the
delimited and Parquet grids offering an offset their sources answer no faster.

## Consequences

- **A selection cannot span pages, and moving pages clears it.** Row indices are
  page-relative, so a block held across a page move would name different rows;
  `blockAsText()` clamps to the page the way it already clamped to the model.
- **The footer is the only place a row's number within the whole table appears.**
  Everything else — the cursor arithmetic, `copyBlock()`, the view — works in the
  coordinate system it already worked in.
- **Anything that changes what is being shown returns to the first page:** a new
  source, a different table, a change of filter, and `refresh()` when the page it
  was on no longer exists. The delimited viewer calls `refresh()` while rows are
  still arriving from an import, and a page can vanish under it.
- **The word *page* now means one thing.** The five-hundred-row fetch inside the
  model is a *chunk*; a *page* is what the reader is looking at.
- **The total can arrive after the first frame** — see MOLE-186, where counting a
  SQLite table moved off the interface thread — so the strip shows its range
  without a total and adds the total when it turns up.
