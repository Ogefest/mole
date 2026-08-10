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

- A `.mole-partial` file, on a remote drive or a local disk, is the wreckage of a
  write that was killed before it finished — see [ADR-0020](docs/adr/0020-an-upload-in-progress-wears-a-different-name.md)
  and [ADR-0021](docs/adr/0021-the-working-name-is-not-only-for-servers.md).
  **Deleting them is a manual job**, and deliberately so for now: they are
  visible in any listing, which is most of what a sweep would have bought, and
  the version that decides on its own which leftovers are safe to remove needs to
  be sure it is not looking at another Mole's transfer in flight.

Tracked as issues rather than kept here: the full-screen window geometry (#31),
FTP staging (#34), WebDAV against a real server (#35), NFS and SMB (#36), video
preview (#37) and the terminal on Windows (#38).
