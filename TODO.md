# TODO

Work that is agreed but not yet built. Finished items move to [DONE.md](DONE.md)
with a one-line note on how they were resolved, so this file stays a list of
what is left rather than a history.

Everything in this repository is written in English — it is an open source
project, and a contributor should never hit a wall of text they cannot read.

---

## Features

### User documentation

A directory of user-facing documentation, kept up to date: what this is, what it
does, and the main features, with screenshots so it is worth looking at. The
screenshots come from `make screenshots`, which photographs states the tests have
just asserted — so the documentation cannot drift into showing something that no
longer works.

### Ctrl+F needs to be usable as a search box

Three things are wrong with the search tab, and only the third is a feature.

Opening it with `Ctrl+F` leaves the keyboard elsewhere, so the first thing anyone
does is reach for the mouse to click into the field. It should be focused, with the
cursor in "Name contains", ready for typing. Enter already starts the search --
`onAccepted` is wired to `controller.start()` -- but nothing about the view makes it
clear that a search is *running*, and a tree walk over a large disk takes long
enough that silence reads as nothing having happened. `LiveSearchController` already
publishes `running` and `statusText`; what is missing is the view making something
of them where the results will appear, the way the file pane and the table preview
now do.

Then the criteria. Today it is a name fragment, an extension and a case-sensitivity
tick. Size belongs there — bigger than, smaller than, between — and once there is
somewhere to put it, so do modification time and possibly type. Worth designing the
form so a fourth criterion does not mean rearranging it again: an "advanced" section
that is collapsed until wanted keeps the common case one field and one key.

The interesting part is the index. `IndexSearchController` exists as its own tab on
`Ctrl+Shift+I`, over indexed volumes, and it is enormously faster than walking a
tree. If the path being searched is inside something already indexed, `Ctrl+F`
should answer from the index instead of scanning — with a toggle to force a real
scan, because an index is only as fresh as its last run and sometimes the truth on
disk is the point. That means the two search features stop being separate strangers,
which is an architectural change and wants an ADR: which one owns the form, how a
path is matched to an index, what the toggle is called, and what happens when the
index covers only part of what was asked for.

### Search results need to arrive sooner, and be worth something once they do

Two halves, and the first is about latency rather than features. A tree walk finds
matches early and the view should show them early: results as they are found, the
same way the table preview now fills as it reads, rather than a wait followed by a
list. Whether that is already happening or the view is batching them is the first
thing to measure.

The second half is what you can then do with them. A search over a large tree
returns more than anyone wants to read, so the results need filtering in place --
narrowing what is already found, without walking the disk again. And they need a way
out: building a file set from the results, so the work continues over that set
instead of ending when the tab is closed. The sets feature already exists and its
view is the place that work continues, so this is a bridge between two features
rather than a new one.

Design question worth settling before the code: whether the set is a snapshot of the
matches at that moment or something that re-runs the query. A snapshot is what "build
a set from this" means to anyone reading it, and it is the one that cannot surprise
you later, but say so explicitly rather than leaving it implied.

### Previews need options of their own, remembered per file type

An `.html` file previews as source, coloured, and sometimes that is exactly what
someone wants — but sometimes they want to see the page. Neither answer is right for
everyone, which makes it a setting rather than a decision.

The strip above a preview already has room for it: it shows the viewer's name and an
OPEN button, so *how this viewer behaves* belongs there too. Choosing "render" for
HTML should apply at once and be remembered, so the next `.html` opens rendered
until it is changed back. HTML is only the first case — this is worth being generic
from the start, because the same question will arrive for other viewers.

There is nowhere to put a preference today. `SessionStore` persists the open tabs and
the window geometry and nothing else: there is no user-preferences store at all. So
the work is three things, and the first is architecture — hence an ADR before the
code:

- somewhere for preferences to live, and a shape for them that is not one hard-coded
  key per feature;
- a way for a preview provider to *declare* its options — a name, the choices, the
  default — so the strip can render them without knowing what any of them mean, the
  same way the menu renders entries it knows nothing about;
- and what the preference is keyed by. Per file suffix is what the request describes
  ("the next `.html`"), but per provider is what the strip is actually showing. They
  differ the moment two suffixes share a viewer, so pick one and say why.

One thing to decide deliberately rather than inherit: rendering HTML through Qt's
rich text engine means letting a document from disk name external resources. A
preview must not fetch them — previewing a file should not put anything on the
network, and a page that could phone home when looked at is a nasty surprise in a
file manager. Whatever the ADR says about the rest, it should say that.

Tests: choosing an option changes the current view immediately; reopening the same
type comes back with the remembered choice; a different type is not affected by it;
the default applies when nothing has been remembered yet; and a rendered HTML preview
reaches for nothing off the disk.

### Compressing files and folders

An action on the selection — one file, one folder, or several of each — that opens
a small dialog for the few things worth asking about: the name of the archive and
which compression to use, with defaults worked out from what is selected. Then it
packs, in the background.

A background job, not a modal wait: a `CompressTask` beside `TransferTask` and
`DeleteTask` in `src/core/tasks`, so it reports progress, can be cancelled, and
appears in the task strip like every other long job. Compressing sixty gigabytes
must not freeze a window.

Both ends go through `IFileSystem`, so what is being packed and where the archive
lands are both just drives — a selection on a remote drive compresses the same way
as one on local disk. It writes a new archive and nothing more: archive mounts are
read-only today and stay that way, because editing one in place is a different
feature with different failure modes.

libarchive is already the archive plugin's dependency for reading inside zip, tar
and 7z, and it writes as well, so this needs no new library. It is an optional
dependency, though, which means the action has to report itself unavailable in a
build without it rather than appearing and then failing — the way `Pty` does on
Windows. Which formats and methods are offered, and what the default is, is worth
an ADR before the code.

The test writes itself, and it is a good one: pack a temp tree, mount the result
with the archive plugin, and compare what comes back against what went in — the
reading and the writing check each other. Cancelling half way must leave no
half-written archive behind.

In the menu this belongs with the operations on the selected files rather than
with the workflows, so it wants the split above to have happened first.

### PDFs have no preview

A PDF falls through to the information viewer — the last resort that describes a
file it cannot show — so previewing one gives its size and its type and nothing of
its contents. The listing already draws PDFs their own icon, and
[src/sdk/IPreviewProvider.h](src/sdk/IPreviewProvider.h) names PDF in its own
description of what previewing is for. It is a stated gap, not an oversight.

What it should be is a page view: pages rendered, scrolling and paging through
them, the same keyboard as the other viewers, and read-only — previewing a
document is not a licence to modify it, which is already the rule for the SQLite
and Parquet viewers. Pages render as they are reached rather than all at once; a
six-hundred-page scan is exactly the case where holding the whole thing as images
would be worst, and the text viewer's windowing is the precedent.

Two candidates, and choosing between them is an ADR before any code, because the
licence audit turns on it. QtPdf is Qt's own (`QPdfDocument`, and a QML
`PdfMultiPageView` that does most of the work) and embeds PDFium; poppler is the
other route and is GPL, which does not sit with shipping Mole under Apache-2.0.
Whichever wins, [docs/LICENSING.md](docs/LICENSING.md) audits that every Qt module
used is LGPL-3.0 and that no GPL-only one is referenced, so the new module's terms
and anything it embeds have to be confirmed against the packaged build, recorded in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and `make licence-check` re-run.

Neither is installed here — there is no `qt6-pdf-dev` and no poppler on this
machine — so the build has to cope with its absence rather than assume it. The two
halves of that pattern already exist: the archive plugin is simply not built when
libarchive is missing, and the image provider claims only what its build can
decode, so an unsupported file falls through to a viewer that can say something
useful instead of opening an empty frame. A PDF in a build without the renderer
should land on the information viewer, exactly as it does today.

A PDF on a remote or archive drive needs a local copy before it can be rendered.
`LocalCopyProvider` exists for precisely that reason — an `<Image>` cannot open
`archive://`, and a PDF renderer cannot either — so this reuses it rather than
inventing a second scratch path.

The tests need a small PDF fixture generated by the test itself: which viewer a
`.pdf` gets in a build with and without the renderer, the page count, a first page
that renders to something other than blank, and a document that is opened
read-only. Then a screenshot in the walkthrough, like the other viewers have.

---

## Notes

- Hit targets: `App.minimumTarget` is the floor for anything that is only an icon,
  and the twenty-four such controls that were below it have been raised. The
  spinners left at 16-20 are deliberate — a `BusyIndicator` is not a click target.
  Two controls carry `objectName`s so a test can hold the floor; the rest would each
  need naming before a tree-wide assertion could replace it.
- The type scale in `AppController` (`textSize`, `secondaryTextSize`,
  `smallTextSize`, `headingSize`, `monospaceSize`) is used by the listing, the
  previews and the sidebar. Around 200 `font.pixelSize` literals remain in the
  other views; they adopt the scale as those views are touched. Nothing new should
  add a literal.
- Video preview is still a documented gap: it needs `qt6-multimedia-dev` and
  `qml6-module-qtmultimedia`, neither of which is installed here.
- Qt's Markdown importer mangles a table placed directly after a blockquote or a
  fenced code block: both end with a stray empty block that lands inside the
  table's first cell, so its header loses the bold and gains an empty line. After
  a paragraph, list, heading or rule it is fine. It happens before the preview
  styling runs, and repairing it would mean editing the document's structure,
  which [ADR-0001](docs/adr/0001-markdown-preview-typography.md) rules out. Left
  as it is unless someone hits it in a real file.
- Backends not yet written: SFTP, S3, WebDAV, NFS, SMB. The conformance suite is
  ready for them — a new backend's test file is a few lines that build a context
  and call `runFileSystemConformance()`, and it now also checks that a backend
  either reports access properly or admits it cannot.
- The terminal panel is Unix-only. Windows needs ConPTY, which is a different API
  entirely; `Pty` reports itself unavailable there rather than pretending.
- Parquet writing is out of scope. Reading a file is not a licence to rewrite it,
  and the same goes for the SQLite viewer, which opens read-only.
