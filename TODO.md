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

---

## Notes

- Streaming search results still resets the model on every batch, which throws away
  the view's scroll position and the highlighted row. It is no longer slow (see
  DONE.md, "A long search froze the interface") but it is unsatisfying to scroll
  through while results arrive. The fix is `beginInsertRows` per run of new rows
  instead of a reset.
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
