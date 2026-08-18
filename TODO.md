# TODO

**Work is tracked on the Mole board in Vikunja**, self-hosted on the author's own
network — see [ADR-0022](docs/adr/0022-work-is-tracked-in-vikunja.md). Anything
actionable lives there: bugs, features, and the tests that are owed. A larger
effort is an epic, which names what several tasks add up to; all of it sits on one
board. The reasoning behind an effort is not kept in the repository — a task
carries what a builder needs.

A task's number is the number the GitHub issue had before the move, so a `#12` in
this file or in the git history is `MOLE-12` on the board. The GitHub Issues tab
stays open as a way in from outside, but it holds no work.

What stays in this file is the other thing — **context that is not a task**.
Behaviour we know about and have decided to live with, conventions a change
should follow, and gaps that are documented rather than scheduled. None of it is
work waiting to be picked up, so none of it belongs in a queue.

Finished work is recorded in [DONE.md](DONE.md), which keeps the long account of
what was asked for and what the answer turned out to be — the part an issue
tracker holds badly.

Everything in this repository is written in English — it is an open source
project, and a contributor should never hit a wall of text they cannot read.

---

## Notes

- **A git status walk cannot be interrupted part way, only abandoned.** libgit2 offers
  no hook inside the stat pass: `git_status_list_new` does the whole of the work
  before the first entry is available, and `git_status_foreach_ext` is that same call
  followed by a loop, so a token polled in its callback never interrupted the walk
  either. Cancellation is checked per entry, which means an abandoned walk stops
  *carrying* its answer rather than stops working. On a very large checkout a pane
  navigated away from still pays for the walk it started; what it does not do is show
  it, or make anything wait. Living with it because the alternative is Mole doing its
  own stat pass, which would be slower than libgit2's and wrong in different ways.
- **A file git calls deleted has no row in a listing, so nothing marks it.** Five of
  the six git states land on a row somebody can see; `D` cannot, because the file is
  not on disk. The deletion is still counted on the band and still rolls up onto the
  folder that held it. Whether to synthesise a row for it is MOLE-184 — a decision
  about what a listing is, not a marker that was forgotten.
- **A drag that started inside Mole is refused by a check no test can reach.** The
  pane's `DropArea` ignores a drop whose `drag.source` is not null, because pane to
  pane is `F5` and `F6`; and a synthetic drop cannot be given a source, since
  `QDropEvent::source()` answers from the `QDrag` that is in flight and there is no
  way to have one without a platform and a pointer. The damaging half of that rule
  is held one layer down instead: `dropHere()` leaves out rows that are already in
  the destination folder, which is what a drag onto the folder it came from
  amounts to, and `tst_BrowserPaneController` asserts it.

- **The guide's pictures are in the locale of whoever ran the suite.** Sizes,
  numbers and dates go through `QLocale`, so the details panel in
  `23-preview-file-info.png` currently reads *"wtorek, 10 marca 2026"* in an
  English-only repository. Fixing it means giving the walkthrough a fixed locale,
  which rewrites every picture that shows a size — so it is one change, made once,
  rather than something to slip into an unrelated ticket.
- **`make guide-images` rewrites all forty-five pictures, whatever changed.** The
  sidebar shows the machine's real free space, so every window in the guide carries
  a number that is different on every run and on every machine. Copy in the pictures
  that changed for a reason and `git checkout` the rest — committing the lot means
  committing forty-odd pictures of your own disk. If this ever becomes worth fixing,
  the fix is a harness option that gives the walkthrough fixed drive figures rather
  than the machine's.
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
- Qt's Markdown importer mangles a table placed directly after a blockquote or a
  fenced code block: both end with a stray empty block that lands inside the
  table's first cell, so its header loses the bold and gains an empty line. After
  a paragraph, list, heading or rule it is fine. It happens before the preview
  styling runs, and repairing it would mean editing the document's structure,
  which [ADR-0001](docs/adr/0001-markdown-preview-typography.md) rules out. Left
  as it is unless someone hits it in a real file.
- Parquet writing is out of scope. Reading a file is not a licence to rewrite it,
  and the same goes for the SQLite viewer, which opens read-only.
- **A window with lines too long to lay out is folded, but only where a line break
  is what makes a block** — the plain text and source case, which is where the
  minified exports, one-line logs and base64 blobs are. A Markdown file or an HTML
  page with no line breaks in it and no markup to divide it either would still
  reach the layout as one enormous block, and folding would not help: both parse
  their own blocks out of the markup, and a newline inside a paragraph is turned
  back into a space by both. Nobody has hit it — a minified `.md` is not a thing a
  tool produces — and the answer if they do is to divide the block, not to insert
  line breaks the renderer discards.

- A `.mole-partial` file, on a remote drive or a local disk, is the wreckage of a
  write that was killed before it finished — see [ADR-0020](docs/adr/0020-an-upload-in-progress-wears-a-different-name.md)
  and [ADR-0021](docs/adr/0021-the-working-name-is-not-only-for-servers.md).
  **Deleting them is a manual job**, and deliberately so for now: they are
  visible in any listing, which is most of what a sweep would have bought, and
  the version that decides on its own which leftovers are safe to remove needs to
  be sure it is not looking at another Mole's transfer in flight.

- **Unmounting a drive does not stop transfers already running on it.** A task
  holds its backend for the length of its run, so a copy in flight finishes
  against the drive it was given while the mount disappears from the sidebar.
  That is the safe half — nothing is pulled out from under a worker thread — and
  it is checked in `tst_TransferTaskUnderFault`. What is missing is the other
  half: nothing tells the user that the drive they just removed is still being
  written to. Left as it is until somebody hits it.
- **Sync does not ask whether a read ended early**, the question
  [ADR-0027](docs/adr/0027-a-read-that-ends-early-is-not-a-file-that-shrank.md)
  added to `TransferTask`. Its copy loop has a worse version of the same fault
  and is tracked as MOLE-98.

Tracked as issues rather than kept here: the full-screen window geometry (#31),
FTP staging (#34), WebDAV against a real server (#35), NFS and SMB (#36), video
preview (#37) and the terminal on Windows (#38).
