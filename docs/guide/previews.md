# Looking inside files

`F3` opens a preview of the file under the cursor. One tab is reused, so previewing
twenty files does not leave twenty tabs behind, and `←`/`→` step through the folder
without going back to the listing.

Whichever viewer suits the file claims it. The strip along the top says which one you
got, and offers whatever that viewer lets you choose.

## Text and code

![JSON, coloured](images/03b-preview-json.png)

Source is coloured lexically rather than parsed, which means a half-written file, a
truncated one, or the middle of a 100 GB log still gets colour — the cases where it
helps most.

Nothing is ever read whole. Only the window on screen is fetched, so a huge log opens
as fast as a small one:

![Paging a large file](images/03c-preview-paging.png)

`Ctrl+PgDn`/`Ctrl+PgUp` move the window; the slider jumps to a point in the file.

## Markdown

![Markdown](images/03c-preview-markdown.png)

Rendered as a page rather than shown as source: headings with room around them, prose
with line spacing, code on a slab, and a capped measure with the gutters taking the
surplus so the line length stays readable on a wide window.

## HTML, either way

Sometimes you want the page and sometimes you want the source, so it is a choice rather
than a decision — and it is remembered for the next file of that type.

![HTML as source](images/03e-preview-html-source.png)
![HTML as a page](images/03f-preview-html-rendered.png)

A rendered page fetches **nothing**. Images, scripts, stylesheets and frames are removed
before it is shown: previewing a file must not put anything on the network, and a page
that could report being looked at is a nasty surprise in a file manager rather than a
feature.

## Tables

![A CSV as a grid](images/02-preview-csv.png)

A delimited file becomes a grid backed by a scratch database rather than parsed into
memory, so there is no row cap: the view scrolls the whole file and the filter searches
all of it, not the part that happened to load. The separator is detected and stays
editable, because detection is a guess and you can see when it guessed wrong.

Rows appear as they are read:

![Still reading](images/02b-preview-csv-loading.png)

For the gap before the first rows arrive — long on a slow drive — it says it is
reading. Once rows are on screen they are the better answer to "is this working".

![Filtering a table](images/04b-table-filter.png)

## Documents

![A PDF](images/03d-preview-pdf.png)

PDFs open as a column of pages, rendered one at a time as they are reached, so a
six-hundred-page scan costs the first page rather than six hundred. Read-only:
previewing a document is not a licence to modify it.

## Anything else

A file no viewer can show gets described instead — its size, its kind, what is known
about it. "Nothing happens" is never the answer.
