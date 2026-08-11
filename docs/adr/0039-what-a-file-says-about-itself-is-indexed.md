# ADR-0039: What a file says about itself is indexed; what is in it never will be

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

The index held a name, a path, an extension, a size and a modification time.
Nothing about what is inside the file. So *the photographs I took with that
camera*, *the documents this person wrote*, *the videos longer than an hour*
could not be asked at all — not slowly, not at any price.

MOLE-151 built content search and deliberately did **not** index anything for
it. That looks like two positions until the sizes are put side by side, and then
it is one: a camera, a lens, an exposure, a date taken and a pair of dimensions
are a few dozen bytes; the photograph is eight megabytes. **The asymmetry is the
whole argument.** An index of the metadata is a few per cent of a disk's worth
of facts; an index of the contents is the disk again.

## Decision

**Metadata is indexed. Contents are not, and this is the record that says so.**

- **Schema version 3** — one narrow table, `file_facts(file_id, key, text, num)`,
  with an index on `(key, text)` and one on `(key, num)`. Key and value rather
  than a column apiece, because the fields come from readers that plugins may
  add and a schema migration per new EXIF tag is not a design. A number goes in
  its own column so a range is a range in SQL; a fact is written to whichever
  column fits and to both when both mean something — an exposure is text to read
  and a number to compare.
- **The keys are namespaced, stable and an interface.** The built-in readers
  hand out:

  | key | from |
  |---|---|
  | `image.camera`, `image.lens` | EXIF make and model, EXIF lens |
  | `image.iso` | EXIF sensitivity, as a number |
  | `image.taken` | EXIF date taken, as seconds since the epoch |
  | `image.width`, `image.height` | the decoder's size, as numbers |
  | `doc.author`, `doc.title` | PDF, `.docx` and OpenDocument properties |
  | `doc.pages`, `doc.words` | the same, as numbers |
  | `media.duration` | a container's duration in seconds, as a number |
  | `media.codec` | the codecs named in the container |
  | `audio.title`, `audio.artist`, `audio.album` | the tags |

  Name one once and never rename it: it is what a form offers and what a saved
  query holds.
- **Filled by the readers that already exist.** `IMetadataReader` — the fifth
  extension point, which fills the details panel — gained a `key` and a `number`
  on `FileFact`. A fact without a key is shown and not asked about, which is
  most of them. **No second parser**, which is what stops the panel and the
  index ever disagreeing.
- **Optional, per scan, with the cost stated in files.** Off by default. A scan
  without it writes exactly what a scan wrote before any of this existed, and
  the dialog says what turning it on means: one read per file.
- **Failures are ordinary.** A file whose readers find nothing is indexed
  without facts; a reader that gives up costs its own rows and never the scan.

## Reason

**Key and value rather than a column per field.** A column per field means a
migration per field, and the fields arrive from plugins. It also means a table
of ninety mostly-null columns for a tree of photographs and text files. The cost
of the narrow table is one join, which is exactly what the two indexes are for.

**Both a text and a number for one fact**, because the display form and the
comparable form are different and neither is derivable from the other by anybody
but the reader that produced it. `ISO 400` sorts wrongly as text and reads badly
as `400`.

**The key on the fact rather than a table mapping labels to keys.** A label is
prose and a key is an interface; deciding them in two places means matching them
up by string later, which is the kind of coupling that breaks silently when
somebody improves a label.

**Rejected: indexing the contents too.** Not on cost grounds alone — on the
grounds that it changes what the index *is*. Today it is a catalogue that fits
beside the files; with the contents in it, it is a second copy of them, and
every question about how big it may grow becomes a question about the disk. The
asymmetry above is the whole reason metadata is on the other side of that line.

**Rejected: reading metadata on every scan.** Bounded per file and unbounded in
aggregate: a hundred thousand photographs is a hundred thousand reads. The
choice belongs where the number of files is known, which is where the scan is
started.

## Consequences

- The index schema is version 3. Version 2 was the scan generations
  ([ADR-0035](0035-a-scan-is-swapped-in-not-cleared-and-refilled.md)), so this
  is the second migration and follows the same append-only rule. An index
  written by the previous version gains an empty table and loses nothing;
  `tst_IndexDatabase` winds a database back and opens it to hold that.
- `FileFact` grew two fields with defaults, so no plugin outside this repository
  has to do anything — and a plugin that wants its facts searchable does one
  thing: name them.
- A metadata criterion is answered by the index where a volume was scanned with
  it on, and by a bounded read where it was not. The same readers either way, so
  the two sources cannot disagree; the difference is what it costs, which is
  what `PredicateCost` exists to say.
- The scan's progress counts files read as well as entries indexed, because with
  this on that is where the time goes.
