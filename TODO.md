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

Finished work is recorded in the commit that did it: what was asked for on the
ticket, what the answer turned out to be — including where the first answer was
wrong — in the commit message, a decision about shape in an ADR, and one line per
user-visible change in [CHANGELOG.md](CHANGELOG.md). See
[ADR-0071](docs/adr/0071-the-record-of-finished-work-is-the-commit.md).

Everything in this repository is written in English — it is an open source
project, and a contributor should never hit a wall of text they cannot read.

---

## Notes

- **The write-ahead log grows a little faster now that a read does not wait for a
  write, and nothing collects it explicitly.** Measured over 2,000 short
  transactions with a thread doing nothing but read: under the single lock the log
  peaked at 4,268 KiB, the same as a writer with no reader at all, because a
  serialised read can never hold a snapshot across a commit. With the two locks of
  [ADR-0065](docs/adr/0065-the-index-serialises-writers-and-lets-readers-through.md)
  it peaked at 4,888 KiB — 14.5% more — and that was a reader running flat out,
  129,154 reads, where the interface makes a handful. So the mechanism is real and
  the magnitude is small, which is why it is a note and not a task. If it does
  become a problem the answer is an explicit `wal_checkpoint(TRUNCATE)` at a quiet
  moment and **not** a return to one lock, which would bring back the sixty seconds
  the split was for. The reproduction machine already carries a 152 MB log against
  a 734 MB database, so the thing to watch is whether that ratio moves.

- **An emoji in the interface is a resampled bitmap, not an outline, and that costs
  determinism.** No outline font on this machine covers the astral plane, so 📄 📁 🎵 and
  the rest of what `FileListModel` and the sidebar draw all fall back to Unifont Upper,
  a bitmap font, and are resampled to whatever pixel size the label asks for. Something
  in Qt's render path flips between two states about one run in six; with an outline
  glyph that flip costs one level out of 255 and with a resampled bitmap it costs
  thirty-seven — enough to be seen, and enough to rewrite a picture in the guide.
  MOLE-266 measured it over twenty runs and moved the two dialogs that showed it to ▤
  and ▦ from DejaVu.

  **The file listing still uses emoji throughout and is stable in every picture**, so
  this is not a rule that every emoji must go. What it means is that an emoji in a
  *dialog* — anything that scales, animates, or is built while something else is
  happening — is a glyph whose pixels nobody has promised. If another picture starts
  moving in a 7-by-10 patch, this is the first thing to check.


- **`click(centreOf(item))` reads a position and clicks it at a different instant,
  and four call sites still do it.** `tests/app/tst_SetsTab.cpp` and three places in
  `tests/app/tst_Dragging.cpp` click or double-click a position taken from an item.
  That is the shape of MOLE-256: an item that has just become visible reports where
  it was before the layout pass placed it, and under load the click goes to whatever
  is really there — in that case the pane's back button, which navigated away and
  left the test saying the thing it clicked had not opened.
  `QmlAppHarness::clickOn()` reads the position until two consecutive reads agree
  and is what a new test should use. The four that remain are clicking items that
  have been on screen for a while rather than ones that just appeared, so they are a
  latent risk rather than a known fault — and converting them blind would be
  changing four passing tests on a hunch. `clickOn` has no double-click twin yet,
  which is what `tst_SetsTab` would need.

- **Nine of the guide's pictures cannot be identical twice running, and they are
  named in `scripts/check-screenshots.sh`.** Three are of something in motion on
  purpose — a folder still loading, a CSV part-read, a transfer in flight — and
  photographing the settled state instead would lose the point of the picture. Three
  carry a duration or a timestamp as their *content*: a run's row saying "26 ms" one
  time and "16 ms" the next is the row working correctly, and freezing the clock
  application-wide would mean a test seam in thirty-eight call sites across twenty
  files, which is not proportionate to a picture.

  The seventh, `26-indexes`, is the odd one: it moved once, by 6652 pixels, and did not
  recur in eight further pairs of runs — two of them under a load average above seven.
  The only clock in that view is the row's "scanned just now", which `ageInWords()`
  turns into "2 minutes ago" at exactly 120 seconds; the step scans and photographs
  well inside that even under load, so that is a mechanism without evidence rather
  than a cause, and it is not written down as one. **It is the only name in that list
  which is not a claim about the picture, and it should stay the only one** — a list of
  "we do not know" is a list nobody can act on either.

  What changed is that a second sighting will explain itself. `compare-shots` now
  prints the box the differences fall inside, and `check-screenshots.sh` copies both
  versions of anything that moved into `build/debug/screenshots-changed` instead of
  letting the two temporary directories take them away. That is what was missing the
  first time: the run said *6652 pixels* and nothing else, and by the time anybody
  asked what had moved there was nothing left to look at.

- **`make screenshots-check` compares what an eye can see, not bytes, and that is
  a measurement rather than a preference.** With every cause fixed, two runs still
  differ by one to five levels out of 255 in a few dozen pixels of pictures whose
  content is letter-for-letter the same: Qt's scene graph does not render a frame
  to identical bytes twice. Most of them do come out byte-identical and *which* few
  do not moves between runs, so a byte comparison
  with a fixed exception list would be flaky — and a flaky check is worse than
  none. See [ADR-0063](docs/adr/0063-the-guides-pictures-are-compared-by-eye.md).

- **A picture can leak what a text file would never be allowed to.** `10-terminal.png`
  carried a real user name and a real machine name into a public repository for as
  long as it existed, because the terminal panel starts `$SHELL` and that was
  whoever ran the suite. The panel now starts a shell with no rc files and a prompt
  of its own. Worth remembering when adding a picture of anything that shows an
  environment: the rule in CLAUDE.md about host names applies to pixels too.

- **The sweep spares a run on another machine by its open files, not by its pid.**
  A payload is named `mole-<what>-<pid>`, and that pid belongs to the test binary
  on whoever's machine started the run — it means nothing on the server. So
  `mole-control sweep` is *told* which pids to spare, and `test-heavy.sh` and
  `test-live.sh` tell it what `pgrep` finds locally. Two tiers started side by side
  on one machine are covered by that; two started from *different* machines are
  covered only by the second rule, which is that anything a server currently holds
  open is a transfer in flight and survives. A payload that a run has stopped
  writing to but has not finished with — between two cases, say — could still be
  taken by a sweep started elsewhere. Nobody runs the tiers from two machines
  today, and the fix would be a lease file rather than a cleverer pattern.

- **The FTP root on the testbed holds about three megabytes the sweep will not
  touch.** `f16-<pid>.bin`, `fresh-<size>-<pid>.bin`, `probe-<size>.bin`,
  `t-<pid>.bin`, `rangeprobe.bin` and friends, left by suites that have since been
  renamed. They predate the `mole-` convention, so no pattern the sweep could
  reasonably carry would match them without also matching other people's files —
  and carrying dead patterns for code that no longer exists is how a sweep becomes
  something nobody trusts. They are removed by hand, once.
  `tst_MoleControl.sh` now fails if any suite invents a pid-stamped name outside
  the convention, so the situation cannot recur.

- **`shellcheck` is not in the suite, and the rules it would enforce are held by
  hand instead.** `shellcheck` flags the class of fault MOLE-233 was about — a
  `\$` on a line that runs in the outer shell rather than in a heredoc — and is
  the better long-term answer to it. It is not installed on this machine and is
  not a build dependency, so committing a `make` target for it would have meant a
  gate nobody here could run. `tests/scripts/tst_ShellScripts.sh` holds the rules
  that matter instead: every script parses, every script sets `-u`, no line that
  runs locally defers expansion to a machine, and no private address is written
  into a tracked file. If `shellcheck` is ever taken on as a dependency, the first
  three of those become redundant and should be dropped rather than kept beside
  it — see [ADR-0062](docs/adr/0062-shell-scripts-are-tested-by-stubbing-ssh.md).

- **A shell test asserts what was sent to a machine, not what the machine does
  with it.** The stub `ssh` in `tests/support/shelltest.sh` records the script it
  would have run and exits 0, so an export line that is correctly formed and wrong
  about how `exportfs` behaves passes. That question belongs to
  `scripts/testbed/check-services.sh` against a real machine, and the two answer
  different things — the same split as the live suites.

- **A changed host key is the one interference case a fake cannot mirror.** Every
  other case in the interference tier has a cheap twin that runs on every change:
  a connection dying mid-read or mid-write, a service that goes away and comes
  back, a link that keeps handing back less than it was asked for, a destination
  that fills up, a process killed outright. A rotated host key has no twin,
  because a fake filesystem has no host key to change and the refusal being
  tested belongs to the SSH layer rather than to us. `tst_Interference::aChangedHostKeyIsRefused`
  is therefore live-only and stays that way. It is the one place where "the
  cheapest place that can hold it" is a server.

- **Mole shows git state and does not change it.** No staging, no committing, no
  checking out, no branching, no fetching, no discarding. Decided rather than pending:
  a file manager that *shows* git state is useful to everybody with a checkout, and one
  that half-implements a git client is a worse `git` and a worse Mole at the same time —
  it would have to grow conflict resolution, hunk selection and credential handling to
  be worth reaching for, and anybody who wanted those already has better. The read-only
  boundary is what keeps this feature small enough to be correct. See
  [ADR-0041](docs/adr/0041-git-state-is-read-through-libgit2.md).
- **A repository on a remote drive shows nothing, and that is not a gap to be filled by
  stretching this feature.** libgit2 wants a path a kernel understands, and pulling
  `.git` across SFTP to decorate a listing would be slow, wrong when it went stale, and
  unbounded in what it fetched. If reading a remote checkout is ever wanted, the honest
  form is a git backend behind `IFileSystemFactory` — a drive that speaks git — rather
  than this feature reaching across a network. That composes with everything here and
  needs none of it changed.
- **There is no diff and no log viewer, and neither belongs in the band.** Both are
  *previews* of a file or a commit, so the shape they would take is an
  `IPreviewProvider`, which composes later without touching anything built for the band
  or the markers. Left out rather than deferred: the band answers "what state is this
  folder in", and reading a diff is a different question that already has a good answer
  one keystroke away in the terminal panel.
- **"3 days ago" is spelled out in four places.** `ReportsFeature`, `BrowserFeature`,
  `SearchFeatures` and now `RepositoryInfo` each have their own copy of the same
  relative-time formatting, and the wordings have already drifted apart slightly. Not
  extracted yet because each one's exact strings are asserted by its own suite, so the
  extraction is a change to four sets of expectations rather than a move. Whoever next
  has reason to touch one of them should take the other three with it.
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
- **`make guide-images` rewrites every picture, whatever changed.** The
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
- **Two threads stamping a listing's times race inside glibc's timezone cache.**
  ThreadSanitizer says so when the conformance suite's concurrency case runs against
  a real server: `QDateTime::fromSecsSinceEpoch` reaches `localtime_r`, and glibc's
  `tzset_internal` caches what `TZ` said without a lock. It is not ours — no backend
  frame appears in the report — and any backend that stamps a modified time from two
  threads can reach it. It stays documented rather than worked around because the
  first touch decides it: in the application the interface has read a local time long
  before a worker thread does, and nothing here ever changes `TZ`. If it is ever worth
  closing, the fix is to touch the timezone once at startup rather than to lock
  anything.
- **Sync does not ask whether a read ended early**, the question
  [ADR-0027](docs/adr/0027-a-read-that-ends-early-is-not-a-file-that-shrank.md)
  added to `TransferTask`. Its copy loop has a worse version of the same fault
  and is tracked as MOLE-98.

- **Mole has only ever been built and run on Linux.** It is written to be portable
  and most of it is, but the claim that nothing in the codebase is Linux-specific
  was never checked against a compiler that would argue with it, and it was wrong:
  the build presets, the way a local path is spelled inside a `VfsUri`, what counts
  as a drive, what a name is allowed to be, and what `QFileInfo` means by a link all
  differ on Windows or macOS. The work of putting that right is an epic on the board
  — *Windows and macOS, built and run* — and the tickets in it are the list. It is
  deliberately not repeated here or in `README.md`: a list kept in two places goes
  stale in one of them. What belongs here is the standing position, which is that
  the gap is known and tracked rather than a surprise waiting for whoever tries a
  Windows build first.

Tracked as issues rather than kept here: the full-screen window geometry (#31),
FTP staging (#34), WebDAV against a real server (#35), video preview (#37) and
the terminal on Windows (#38).
