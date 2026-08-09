# TODO

Work that is agreed but not yet built. Finished items move to [DONE.md](DONE.md)
with a one-line note on how they were resolved, so this file stays a list of
what is left rather than a history.

Everything in this repository is written in English — it is an open source
project, and a contributor should never hit a wall of text they cannot read.

---

## Features

Nothing agreed and unbuilt at the moment. What is next comes from
[README.md](README.md)'s extension points: video, audio-tag and image-metadata
previews, the backends listed below, and adding to an existing archive rather than
only writing a new one.

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
- **A large SFTP upload has no equivalent of the span loop that fixed reads.** An
  SFTP transfer stops dead a little short of a gibibyte, where an OpenSSH server
  re-keys the session -- see
  [ADR-0013](docs/adr/0013-a-large-sftp-read-arrives-in-spans.md). Reads now arrive
  by byte range, a span per connection, so nothing reaches the fault. There is no
  range for a write: `uploadTo()` gives a large payload a connection nobody has used
  and can do no more, so writing a file over about a gigabyte to an SFTP drive is
  expected to stall the same way. The shape of a fix is `CURLOPT_APPEND` with
  `CURLOPT_RESUME_FROM_LARGE`, a span at a time, and the awkward part is what to do
  when the process dies half way and the server is left holding a partial file that
  looks finished. Untested against a real server; do that first, with
  `MOLE_TEST_SFTP_*` and a file over a gigabyte.
- **`openRead()` stages the whole remote file in the temporary directory before a
  copy writes a single byte.** Every network backend does it -- see
  `net::openDownloadedFile` -- which buys random access for the preview layer at the
  price of needing as much free local space as the file is big, whatever the
  destination. Copying a 94 GB backup off a NAS therefore needs 94 GB free in
  `/tmp` even when the target is a different drive with room to spare, and an
  SFTP-to-SFTP copy stages it twice: once by `openRead`, once again by
  `BufferedUpload`. Nothing reports how far along the staging is either, so a
  multi-gigabyte copy sits at 0% and then jumps. The fix is a streaming read for the
  callers that only ever go forwards -- a copy, a hash, a sync -- keeping the staged
  copy for the ones that genuinely seek.
- S3 multipart upload is not implemented, so a single object is held to whatever the
  provider accepts in one PUT (5 GB on AWS). Anything larger needs the multipart
  API, which is a different shape: begin, N parts, complete, and something sensible
  to do when the process dies half way.
- The terminal panel is Unix-only. Windows needs ConPTY, which is a different API
  entirely; `Pty` reports itself unavailable there rather than pretending.
- Parquet writing is out of scope. Reading a file is not a licence to rewrite it,
  and the same goes for the SQLite viewer, which opens read-only.
