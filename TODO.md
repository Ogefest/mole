# TODO

Work that is agreed but not yet built. Finished items move to [DONE.md](DONE.md)
with a one-line note on how they were resolved, so this file stays a list of
what is left rather than a history.

Everything in this repository is written in English — it is an open source
project, and a contributor should never hit a wall of text they cannot read.

---

## Features

- **A task that knows its speed and its size should say how long is left.** Every
  ingredient is already there and already published: `Task::setBytesDone()` keeps a
  smoothed rate in `TaskMetrics::kRate`, the total is in `TaskMetrics::kBytesTotal`,
  and `TaskMetric::Kind::Duration` exists precisely so the interface can format a
  time without parsing text. So the remaining work is the arithmetic —
  `(total - done) / rate` — published as a metric next to the rate in
  `Task::setBytesDone()`, which means every task that measures bytes gets it at
  once rather than each one growing its own. Points worth deciding before writing
  it: say nothing until the rate has settled, because an estimate from the first
  half-second is worse than no estimate; keep the smoothed rate rather than the
  instantaneous one, or the figure jumps around unreadably; and hold the last
  estimate through a stall instead of showing infinity. `TaskStrip.qml` already
  lays out `activeRateText`, so the strip needs one more field beside it, and
  `TaskListModel` one more property. A copy that has ten thousand files but only
  measures bytes still works — that is what makes bytes the right thing to count.

What is next after that comes from [README.md](README.md)'s extension points: video,
audio-tag and image-metadata previews, the backends listed below, and adding to an
existing archive rather than only writing a new one.

---

## Notes

- A window left full-screen comes back as an ordinary window, and the size it had
  before going full-screen is lost with it. `Main.qml` reports the state to
  `rememberWindowGeometry()` as one boolean, `root.visibility === Window.Maximized`,
  which is false while full-screen; `AppController` therefore treats the window as
  normal and overwrites the remembered x/y/width/height with the full-screen
  metrics, so the next start opens a plain window the size of the screen. The fix is
  a tri-state — normal, maximised, full-screen — carried through
  `WindowGeometry::maximized` in `SessionStore.h`, its JSON either side of
  `SessionStore.cpp`, and the `savedWindowGeometry()` map, with the "keep the
  pre-maximise size" guard in `rememberWindowGeometry()` extended to full-screen as
  well. `tst_Session.cpp` already has the maximised pair to extend. Note that
  nothing in the app enters full-screen itself — the state arrives from the window
  manager, so `onVisibilityChanged` is the only thing that notices it.
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
- Backends not yet written: NFS and SMB. SFTP, FTP, S3 and WebDAV ship in the
  network plugin; SSHFS was dropped rather than written, because a Mole drive is
  virtual and in-application and FUSE would not port to Windows -- see
  [ADR-0011](docs/adr/0011-network-drives-without-rclone.md). The conformance suite
  is ready for the remaining two: a new backend's test file is a few lines that
  build a context and call `runFileSystemConformance()`, and it also checks that a
  backend either reports access properly or admits it cannot.
- WebDAV has never been run against a live server -- there was none to hand when it
  was written. Its PROPFIND parsing is covered offline against Nextcloud- and
  Apache-shaped answers, and `tst_WebdavFileSystem` has a conformance run that waits
  on `MOLE_TEST_WEBDAV_URL`. Point it at a real Nextcloud before trusting it.
- An SFTP upload that is interrupted leaves part of a file on the server. The
  backend deletes what it wrote when a transfer fails -- see
  [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md) -- but a
  process that is killed outright cannot delete anything, and what it leaves looks
  like a finished file. Uploading to a temporary name and renaming on success would
  close it: the rename is atomic on the server, and a leftover
  `name.mole-partial` is obviously not the real thing. The same treatment would
  suit FTP and WebDAV.
- The WebDAV streaming write has never been run against a server, like the rest of
  that backend -- see the note above. A large write now goes out with a chunked
  transfer encoding, which is the only way to send something too big to stage, and
  a server that answers 411 will refuse it. Small writes keep the staged PUT with an
  exact length, so the risk is confined to the case that has no alternative. Point
  it at a real Nextcloud along with everything else.
- FTP still stages a whole upload in a temporary file before sending it, so a file
  larger than the local scratch space cannot be written to an FTP drive. SFTP, S3
  and WebDAV no longer do -- see
  [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md) and
  [ADR-0015](docs/adr/0015-s3-uploads-in-parts-webdav-in-chunks.md). FTP would take
  the same treatment as SFTP: it has `APPEND` and `REST`, so spans and resumption
  both exist. Nobody has been blocked on it yet.
- The terminal panel is Unix-only. Windows needs ConPTY, which is a different API
  entirely; `Pty` reports itself unavailable there rather than pretending.
- Parquet writing is out of scope. Reading a file is not a licence to rewrite it,
  and the same goes for the SQLite viewer, which opens read-only.
