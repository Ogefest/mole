# Looking inside files

`F3` opens a preview of the file under the cursor. One tab is reused, so previewing
twenty files does not leave twenty tabs behind, and `←`/`→` step through the folder
without going back to the listing.

Whichever viewer suits the file claims it. The strip along the top says which one you
got, and offers whatever that viewer lets you choose.

## What a file is

A file's type is decided by **what is in it**, not by what it is called. When the name is
enough — `.png`, `.csv`, `.pdf` — that is the end of it. When it is not, Mole reads the
first four kilobytes and asks again:

![A Dockerfile, coloured](images/03g-preview-dockerfile.png)

So a `Dockerfile`, a `.gitignore`, a `LICENSE` or a `.bashrc` opens as text although no
suffix says so — and where the name is itself the type, it is coloured for it: Dockerfiles,
makefiles and `CMakeLists.txt` as much as anything ending in `.py`. It works the other way
round too: a zip renamed `notes.txt` is not shown as text, because the bytes say what a file
is and the name is only a label somebody typed.

One page is read to decide, whatever the file's size, and nothing else changes: a file that
already had a viewer still gets it, and an empty file is not read at all.

## Text and code

![Plain text](images/03-preview-text.png)

Plain text is drawn at the same size as everything else in the window, in the same
monospaced family every code and data view uses — a log or a source file is the preview
people leave open longest, so it is not the place to save a pixel.

![JSON, coloured](images/03b-preview-json.png)

Source is coloured lexically rather than parsed, which means a half-written file, a
truncated one, or the middle of a 100 GB log still gets colour — the cases where it
helps most.

A file with no line breaks in it at all — a minified export, a one-line dump, a base64
blob — is folded into readable lengths rather than handed to the layout as a single line
half a million characters long, and the header says so: the breaks you are looking at are
Mole's, not the file's. Colouring goes off for such a window, because a fold cuts a
string in half and a half-coloured string is worse than none.

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

## Pictures

![An image](images/20-preview-image.png)

An image is fitted to the pane rather than shown at its own size, so a photograph off a
camera is looked at rather than scrolled around. Whatever Qt on this machine can decode
is claimed — png, jpeg, webp, and the rest of that list — and anything it cannot is left
to the viewer of last resort below.

## Databases

![A SQLite database](images/21-preview-sqlite.png)

A SQLite file opens as its tables and views, one at a time, read a page at a time. Open
**read-only**: reading a database is not a licence to write to it, which is the same
rule the PDF viewer follows.

![A Parquet file](images/22-preview-parquet.png)

Parquet the same, when the build has Apache Arrow — it is optional, and a build without
it says so on the file rather than pretending the file is broken. Column types come from
the file's own schema, and only the row groups needed for what is on screen are read, so
a file of millions of rows opens as fast as one of ten.

## Details

Under the strip of every viewer, **Details** is what the file says about itself:

- a **photograph** — its dimensions, format and colour depth, and what the camera wrote:
  the make and model, the lens, the exposure, the aperture, the ISO, the focal length, when
  it was taken, and the position if the camera recorded one
- a **document** — a PDF's title, author, page count and page size; a `.docx` or `.odt`'s
  author, who saved it last, when, and how many words
- a **video** — how long it runs, how big the picture is, the frame rate and the codecs
- an **audio file** — title, artist, album, year, track and genre, with the duration, the
  bitrate, the sample rate and the channels

The panel is **closed until you open it**, and nothing is read for a panel nobody opened —
so stepping through a folder of photographs stays as quick as it is. Whether it is open is
remembered per file type: EXIF on every photograph does not mean a panel on every log.

Reading it puts **nothing on the network**, which is the same rule a rendered page follows.
A picture's position is shown as the numbers in the file and is not looked up anywhere; a
document's properties are parsed without resolving anything the XML names. And nothing is
read whole to fill it: a header, and at most one further bounded read.

## Anything else

![The bytes of a file](images/25-preview-hex.png)

A file whose format nothing here knows is shown as what it actually is: bytes. The offset,
sixteen bytes in hex, and the same sixteen as text with a dot for everything that has no
printable form. Dragging across either column selects a run of bytes; `Ctrl+C` copies it as
hex and `Ctrl+Shift+C` as text, which is often the whole reason for opening such a file.

It is read-only, like every other preview, and windowed like the text viewer — 64 kB at a
time, with the same paging keys — so a firmware image or a 100 GB disk image opens as fast
as anything else.

![A file nothing else claims](images/23-preview-file-info.png)

What is left is a file with nothing to show at all — an empty one, or one that cannot be
read. It gets described instead: its size, its kind, what is known about it. "Nothing
happens" is never the answer, and this is the reason: something always claims the file,
even when all it can say is what the filesystem knows.
