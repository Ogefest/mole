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

### Sort the F4 menu by what an entry actually does

The menu behind `F4` has everything in one heap. The Tools section holds two
different kinds of thing at once: entries that open a workflow — sync, duplicate
finding, bulk rename, saved reports, alerts, scheduled jobs, drives — and entries
that do something to whatever is under the cursor right now, such as previewing
this file, analysing this folder, indexing it or adding it to a set. Read as a
list they are indistinguishable, so finding anything means reading all of it.

The two need separating and naming: building sets, bulk changes and the other
workflows on one side, and the concrete operations on the selected files and
folders on the other. Choosing the names is part of the work, because the present
headings do not carry the distinction.

This changes an extension point rather than only the shell. Sections come from
`MenuAction::Section` in [src/sdk/MenuAction.h](src/sdk/MenuAction.h), which
every plugin picks from, and that enum is deliberately small — a menu with eleven
top-level headings is a search problem, not navigation. So the split needs an ADR
before the code: what the sections are, what belongs in each, and what a plugin
that guesses wrong should do.

### How big is this folder, answered in the listing

There is no quick way to see what a folder contains. Analysing it gives the answer
and more, but opening an analysis tab is a detour when the question is just "which
of these five folders is the big one".

An action on the active view, then: the selected folders, or every folder in the
listing when nothing is selected, sized in the background and the answer written
into the rows themselves — beside the entry, where the eye already is, filling in
as each total lands rather than all at once at the end. A `Task` like everything
else that takes time, so it reports progress, can be cancelled and shows up in the
task strip.

The walking is already written: `AnalyseDirectoryTask` produces an `AnalysisReport`
carrying `bytes`, `fileCount` and `folderCount`, which is more than this needs but
means there is no second tree-walker to write and keep correct. What is missing is
somewhere for a computed total to live — `FileEntry::size` for a directory is the
inode's own size and must not be overwritten with a recursive total, or the two
meanings become one field that is sometimes a lie — and a column or suffix in the
listing to show it.

Two decisions to make rather than assume. What happens to a computed size when the
listing is refreshed or the folder changes underneath: dropped, kept with a hint
that it is from a moment ago, or recomputed. And whether asking twice recomputes or
answers from what was already measured. Both are worth an ADR only if the answer
turns out to be a cache; if it is "dropped on refresh", a line in the code will do.

The tests are straightforward and worth being strict about: a tree of known size
totals correctly including nested folders, sizes appear on rows progressively rather
than only at the end, cancelling leaves the rows it had already filled and stops,
and a folder the walk cannot read reports what it managed rather than nothing at
all.

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

### Bulk rename hides the thing you are supposed to be reading

`BulkRenameView.qml` opens by stating its own priority — *"The preview is the
feature"* — and then lays itself out as though the form were. The rules column asks
for `Math.max(340, view.width * 0.4)`, the preview merely fills what is left, and
there is not a single `Layout.minimumWidth` in the whole view, so the form's own
content decides how much room the preview gets. The rule editors are grids of
full-width text fields, which means every rule stretches as far as it is permitted:
a two-character prefix gets a box wide enough for a sentence, and the
before-and-after list ends up squeezed into whatever is left.

The preview also does not keep up while you type, and it is inconsistent about it
in a way that is worse than being uniformly slow. The dropdowns update immediately
(`onActivated`) and so do the numbers (`onValueModified`), but all seven text
fields use `onEditingFinished` — so a prefix shows no effect until Enter is pressed
or the focus moves elsewhere. Two behaviours in one form, and the fields carrying
the interesting part are the ones that feel dead. Text should recompute as it is
typed; if that turns out to be expensive on a large batch, debounce it on a
measurement rather than on a guess.

Worth reopening the arrangement itself rather than only its proportions: a compact
strip of rules with a tall preview beneath it may suit a wide window better than
two columns, and a `SplitView` — used elsewhere in the application already — would
hand the decision to whoever is renaming four thousand files. Fields sized to what
they actually hold would remove most of the width problem on its own.

Two things here are testable and should not be left to the eye: that typing into a
rule changes the preview without leaving the field, and that the preview keeps a
usable share of the width at an ordinary window size. Apply already refuses while
any row would collide — that behaviour is right and must survive the rework.

### The small controls need a review, starting with + and ×

The buttons that add a bookmark and close a tab are `ToolButton`s of 22 by 22, and
the drive's remove button is 20 by 20 — with a text glyph inside, so both the
target and the mark it carries are small. They are fiddly to hit and they read as
afterthoughts rather than as the controls for the two things you do most often.

It is not two files. There are 52 explicit `implicitWidth` or `implicitHeight`
values below 24 across 18 QML files: mostly 22, then 18, 20 and 16, and one 14. So
this is a sweep with a decision behind it, not a nudge in the two places that
annoyed someone first.

The decision is a floor for anything that is only an icon — 24 is the figure
usually quoted as a minimum for a pointer, and on a desktop something nearer 28 or
32 stops a close button feeling like a pinprick — plus the rule that the glyph
grows with the button instead of staying at the default text size inside a bigger
box. It shares a cause with the entry below: sizes are decided view by view with
nothing shared to decide them from, so both want the same home — one place holding
the base text size, its steps, and the minimum target.

Two things worth looking at while sweeping, because they are the same review.
Several of these controls appear only on hover — the drive's × is
`visible: row.removable && row.hovered` — and a control you cannot see until you
are already on top of it is a discoverability problem rather than a size one; the
review should say where that is deliberate and where it is not. And the glyphs are
literal characters, so what they look like is at the mercy of the font that
supplies them.

The floor is testable, unlike the look: walk the visual tree in the harness and
assert that no icon-only control is below it. Most of these buttons have no
`objectName` today, so naming them is part of the work — which is what makes the
assertion possible in the first place. Whether the result is *pleasant* is what
`make screenshots` and a human are for.

### The type is too small, and there is no scale to raise

Listings and previews are set smaller than is comfortable to read: a file name is
13 pixels, most of the rest of a listing is 12, secondary text is 11, and in
places it drops to 10 and 9. A file manager is a thing people stare at all day.

Raising it view by view is the wrong shape of fix. There are around 270
`font.pixelSize` literals across 27 QML files — 12 and 11 between them account for
most — so "a bit bigger" is 270 edits, and the next view added will guess its own
size again.

The application already chooses the monospace family once, centrally, so that a
preview, a table and a log line up instead of each guessing; sizes deserve exactly
the same treatment. One base size and a few named steps derived from it — body,
secondary, heading — and this becomes one number to change. The Markdown page
already works that way: the view hands `MarkdownStyle` a single body size and every
margin and heading is a ratio of it, so that page follows the base size for free.

Doing it this way also leaves the door open to making it a preference later, which
is what anyone on a high-density screen or with tired eyes actually wants — but it
does not need to be a preference to stop being too small.

Start where the reading happens: the file listing and the previews. The rest can
adopt the scale as those views are touched, so long as nothing new adds a literal.
What a test can hold is that the views take their sizes from the scale rather than
from numbers of their own; whether the result looks right is what the screenshots
from `make screenshots` are for, and that judgement stays human.

---

## Notes

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
