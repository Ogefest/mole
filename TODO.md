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

- **The text preview reads UTF-8 and UTF-16, and nothing else by name.** The
  decoder is chosen from the byte order mark at the front of the first window --
  `QStringConverter::encodingForData()`, which answers for UTF-8, UTF-16 and
  UTF-32 and costs nothing -- and falls back to UTF-8 for everything with no mark.
  So a **Latin-1 or Windows-1252 file with a byte above 0x7F in it shows a
  replacement character** where that byte is. That is accepted rather than
  overlooked: guessing an eight-bit encoding from content is a heuristic that is
  wrong often enough to be worse than a visible replacement character, and the
  file is still readable. If it ever earns its place, the shape is a choice in the
  viewer strip beside *Rendered / Source*, remembered per file type the way that
  one is -- not a guess. See MOLE-358.

- **Finding the drives is a text read on Linux and a `statvfs` per mount
  everywhere else, and the second of those can still hang a start-up.**
  `SystemVolumes::enumerate()` reads `/proc/self/mounts`, which touches none of the
  filesystems it lists, so a stale NFS or SMB mount cannot stall Mole here any more
  (MOLE-361). Where there is no such table -- macOS, Windows -- the answer still
  comes from `QStorageInfo::mountedVolumes()`, which constructs one `QStorageInfo`
  per entry, and each of those is a `statvfs` that blocks for the kernel's timeout
  on a mount that has stopped answering.
  **What it would take:** the platform's own call (`getmntinfo` on macOS,
  `GetLogicalDriveStrings` plus `GetVolumeInformation` on Windows), or the whole
  enumeration on a task. The task was tried and taken out again: mounting the
  drives as the answer arrived left eleven suites failing or timing out, because a
  drive list that is empty for the first instant of a run is a different
  application from the one they were written against. Neither half can be measured
  from this checkout -- see `needs-windows` in CLAUDE.md and MOLE-253.

- **Two names for one inode are still reported as duplicates of each other, and
  the "could be freed" of such a group is a fiction.** A duplicate scan now leaves
  symbolic links out and never compares a file with itself (MOLE-341), but a *hard*
  link is indistinguishable from a copy at the level the scan works at: same size,
  same bytes, same hash, two names. An `rsync --link-dest` snapshot tree -- the
  ordinary NAS backup layout -- is therefore still one enormous set of "duplicates"
  that would free nothing at all, and deleting one name of a pair frees nothing
  while looking as though it freed the file.
  **What it would take:** an identity on `FileEntry` -- device and inode, or the
  platform's file id -- filled in by every backend that has one. That is a syscall
  per entry in every listing on every drive, paid by everything that lists a folder,
  for one feature's benefit; Qt exposes no such field, so each backend would need
  its own call. Worth deciding deliberately rather than adding on the way past,
  which is why it is here and not in the code.

- **Links are copied, never compared, and a sync leaves one that is already at the
  destination alone.** ADR-0092 settled what a copy, a move and a sync do with a
  symbolic link: the link travels, nothing is followed into one, and a drive that
  cannot hold one refuses it by name. What is deliberately not answered is what a
  *changed* link should do. A sync that finds a link already at the far end plans a
  skip with a reason, because "the same name, pointing somewhere else" would mean
  reading both targets and calling a difference in text a difference in content --
  which is defensible and is nobody's request yet. The failure it avoids is the
  worse one: replacing a link at the destination on a guess. **What it would take:**
  `readLink()` on both sides in `differenceBetween()`, and a decision about what a
  link pointing at a path that only exists on one of the two machines means.

- **Two drives cannot honour ADR-0020's rule about a destination that appeared
  while a write was in flight, and that is a fact about them rather than a gap
  waiting to be filled.** The rule needs a working name to finish from: a write
  goes under `.mole-partial`, and the commit compares what is at the destination
  now against what was there when the write began. `s3://` has none — a PUT lands
  on the key, so an object that appeared during the upload is overwritten with no
  moment at which the backend could have looked — and neither does `mem://`, which
  writes straight into its map. Both say so by setting
  `ConformanceContext::stagesWrites` to false, and the shared suite skips that one
  case for them rather than being quietly weakened for everybody. Every other
  drive is held to it: NFS and SMB were exempt for months because their commits
  were private copies that always replaced, which is what MOLE-346 was.

- **`v0.1.1` is a tag with no release behind it, and the gap in the version numbers
  is deliberate.** The 0.1.1 cut passed all three tiers, wrote its commit and pushed
  its tag, and then `release.yml` refused it six steps before `Publish`: the AppImage
  step still demanded `video codecs it reports:` from an artefact that MOLE-322 had
  deliberately stopped carrying codecs in. Run 33608556641. MOLE-330 fixed the check.

  **The tag was left where it is rather than moved**, which was the author's call
  between the two: re-pointing a pushed tag is the kind of thing somebody later
  cannot tell happened. So 0.1.1 exists, nothing was ever published under it, and
  **0.1.2 carries what it would have** — the three changelog entries that had been
  filed under 0.1.1's marker were put back above it, because the changelog is the
  release notes and nothing went out as 0.1.1. Anybody reading `git tag --list` and
  wondering why there is no release for one of them is reading about it here.

- **The AppImage runs on glibc 2.34 and upwards, and that is a promise rather than
  a build detail.** It carries its own Qt and its own libraries but links glibc
  dynamically and cannot carry that, so what it was built on decides what it runs
  on: an AppImage built on the newest distribution refuses to start on anything
  older, with a `GLIBC_2.39 not found` that reads to whoever downloaded it like a
  corrupt file.

  So it is built on **AlmaLinux 9**, chosen for what it reaches rather than for
  itself: glibc 2.34 is older than Ubuntu 22.04's 2.35, Debian 12's 2.36 and Ubuntu
  24.04's 2.39, so one artefact covers every distribution from 2021 onwards. It is
  also the oldest image that can build Mole at all — Qt 6.4 is the baseline and
  EPEL 9 has 6.6, while Ubuntu 22.04's own archive stops at 6.2. Measured rather
  than assumed: it starts on 22.04, 24.04 and Fedora 40, and on Ubuntu 20.04
  (glibc 2.31) it says `version GLIBC_2.34 not found`.

  Lowering the floor means finding a Qt 6.4 for something older than AlmaLinux 9,
  which is a piece of work rather than a setting. Raising it — building on
  something newer because it is convenient — silently drops every user between the
  two, which is why the figure is written here and in the release notes and not
  only in `scripts/package-appimage.sh`.

  **What that distribution has not got is part of the same promise, and it is one
  thing rather than the four it first looked like.** This note used to say the
  AppImage had no Parquet grid, no terminal panel and no NFS drives; that was the
  package list in `scripts/package-appimage.sh` being short, not a property of
  AlmaLinux 9. EPEL 9 has `qt6-qtpdf`, `libvterm`, `libnfs` and Arrow, so the
  AppImage has PDF rendering, the full terminal parser, NFS drives and everything
  else — and the build now refuses to be packed if any of them is missing, so the
  next short list fails rather than ships.

  The Parquet grid is the one real absence: EPEL 9 ships Arrow 9.0.0 and no
  `ParquetConfig.cmake` at all, so `find_package(Parquet)` cannot succeed there
  whatever is installed. **Accepted rather than fixed**, and the difference matters:
  the alternative is building Arrow and Parquet from source inside the AppImage's own
  container, which is a third-party build of a large library on the critical path of
  a release that already does five, for one preview. A Parquet file opens as the list
  of facts there, which is what a viewer that declines is supposed to do.

- **The nine viewers, swept for what a file costs once it is on screen.** MOLE-284
  asked each of them one question: can this viewer find out only after it has read
  the bytes that showing them is more work than the window can afford, and if so
  what does it do about it? [ADR-0078](docs/adr/0078-a-viewer-may-decline-a-file-it-has-read.md)
  is the answer to the second half. This is the answer to the first, viewer by
  viewer, so the next fault of this shape starts from what is already known rather
  than from the beginning.

  Five of the nine cannot go wrong, and the reason is worth keeping. The text
  viewer's three modes are listed apart because they are three different costs:

  - **Text, as source or plain text** — the window is 512 kB and a run too long to
    lay out is folded (MOLE-112), so nothing unbounded reaches the layout.
  - **Text, as Markdown** — was the fault that started this. Answered inside the
    viewer by MOLE-283: the window's longest run of table rows is measured before
    it reaches the view, and over the budget the file is shown as source with the
    reason said out loud.
  - **Text, as a rendered page** — the same window through the same document, and
    **the cost is not the same**, which was worth measuring rather than assuming.
    Qt's Markdown importer is quadratic in the rows of one table; its HTML parser is
    not. The worst case a 512 kB window admits is a page that is nothing but table:
    1,682 rows and 23,550 blocks, which is 130 ms in `setHtml()` and 350 ms for the
    first layout on the baseline Qt 6.4.2. Noticeable, bounded, and only reached by
    a reader who asked for the page.
  - **Tables — a delimited file** — imported on a task in 1 MB chunks and
    5,000-row batches, and the grid shows a fixed 5,000-row page
    ([ADR-0045](docs/adr/0045-a-grid-shows-a-page-of-a-table-and-the-page-is-a-fixed-five-thousand-rows.md)).
    Nothing is held whole.
  - **Tables — SQLite** — opened read-only in place, row counts on a task, the same
    page. A page's query does run on the thread that draws, and its offset is
    bounded by the page, which is exactly what ADR-0045 exists for.
  - **Bytes** — a fixed 64 kB window, 4,096 rows.
  - **File information** — the bottom of the ladder. Its facts come from the
    metadata readers, each on a task, and it accepts every file, so it has nowhere
    to decline to and needs nowhere.

  **Video is a different shape and has no answer available.** Decoding is the media
  backend's and happens off the thread that draws, and a refusal now hands the file
  back (ADR-0078). What nothing can catch is a *stall* — GStreamer sitting on a
  stream rather than refusing it — because the viewer has no way to tell a slow
  start from a stopped one, and a timeout would refuse a file that was about to
  play. Left as it is: the window keeps answering throughout, which is the
  requirement, and the only thing lost is the preview.

  **Three of them can go wrong and are on the board**: MOLE-286 for a document page
  rasterised on the thread that draws, MOLE-287 for a Parquet file written as one
  row group being read whole in the same place, and MOLE-288 for what asking to see
  a large image at full size does to the pane.

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
  whoever ran the suite. **The panel still starts `$SHELL -i` with your own rc
  files** — that is the point of a terminal panel, and this note used to claim
  otherwise. What changed is that the *screenshot harness* asks for something
  else: `MOLE_TERMINAL_ARGUMENTS=--norc --noprofile -i` with `SHELL=/bin/bash` and
  a `PS1` of the project's own, so a picture carries no machine's name. Worth
  remembering when adding a picture of anything that shows an environment: the
  rule in CLAUDE.md about host names applies to pixels too. See MOLE-363.

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

- **Six checks were green — or red — for a reason other than the one they were
  written for, inside four days.** Four rules came out of that. Each is enforced
  somewhere now, and none of them was written anywhere a person would look before
  writing the next check. The evidence is in the commits and the second-family
  reasoning in
  [ADR-0081](docs/adr/0081-a-second-distribution-is-built-on-a-clock.md); what
  follows is the part to read first.

  - **A check has to be able to fail, and a missing input is a failure rather than
    a pass.** Held by `tst_Changelog.sh`, which proves each expression matches
    something before asserting that nothing else does, and by
    `tst_ShellScripts.sh`, which fails on a short glob count. `licence-check.sh`
    reported no GPL-only Qt modules in use when what it could not find was a
    `CMakeLists.txt`. Not only checks: `collect_deps` in `make-bundle.sh` skipped
    in silence every library the build machine had not got, and two artefacts went
    out short of five.
  - **Ask something whose answer could differ.** `mole --version` was the release
    workflow's proof that a package worked, and it was green on an `.rpm` and an
    AppImage that had loaded none of the plugins they shipped — and it then turned
    out `--version` could not answer at all on a machine with no display, because
    it was answered after the whole plugin host was up. Blind, and broken in the
    one situation it existed for. The steps ask `--plugins`;
    `scripts/check-artefact-starts.sh` starts the artefact for real, and
    `tst_StartCheck.sh` holds that that check can fail both ways.
  - **Skip the file's own account of the rule.** Three checks read a file's
    explanation of itself as an instance of what they were looking for — a column
    reading `SKIPPED-none` in the heavy tier's own report tail refused a good
    release. Anchored patterns and skipped fences in `scripts/release.sh` and
    `scripts/changelog-block.sh`, and comments skipped in `tst_Packages.sh`.
  - **A fixture that cannot be true is the same fault as a check that cannot
    fail.** `tst_Release.sh` cut four releases without writing a changelog line
    for any of them, which no real cut can do; four cases could not fail for their
    own reason when the account is root, and one asserted that `QTemporaryFile`
    refuses a missing directory, which is a claim about Qt and untrue.
    `test::madeUnreadable()` makes those four skip rather than pass, and the Fedora
    job runs them as an account that can be refused — a machine that is not this
    one is what asks a fixture whether it meant it. The release workflow then
    turned out to be doing it too: it installed the `.rpm` in the container that
    had built it, so it asked whether a package installs on the system that made
    it. A package records the sonames its binaries link, and that question has one
    answer. `tst_Packages.sh` now holds the two images apart (MOLE-389).

- **`make tidy` works now, and the tree is 91 findings away from clean under
  it.** It had no `.clang-tidy` at all, so clang-tidy ran with its default check
  set; its stderr went to `/dev/null`, so a clang-tidy that could not run said
  nothing; and the recipe ended in `|| true`, so the target passed whatever
  happened. A command in that state is worse than no command, because it is in
  the Makefile, the help text and CLAUDE.md, and so looks like something somebody
  is doing. MOLE-390 gave it a chosen check set, made it print, and made it fail
  when it found something.

  What it finds on 2026-09-04, in `src/`: 21 `performance-no-automatic-move`, 20
  `performance-implicit-conversion-in-loop`, 15
  `bugprone-implicit-widening-of-multiplication-result`, 8
  `performance-unnecessary-value-param`, 7
  `performance-unnecessary-copy-initialization`, and a tail of ones and twos —
  91 in all, and none of them a fault anybody has met. **So a red `make tidy` is
  the state of the tree rather than a regression**, until somebody works through
  them; it is not in `make test` and does not gate a commit. Turning a further
  check on in `.clang-tidy` is a piece of work rather than a setting, for the
  same reason.

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
  minified exports, one-line logs and base64 blobs are. Markup that reaches the
  layout as too much work needs a different answer, because folding cannot help
  it: Markdown and a rendered page parse their own blocks out of the markup, and a
  newline inside a paragraph is turned back into a space by both.

  One shape of that has been hit and is dealt with. MOLE-283 was a 238 kB
  Markdown report whose largest table was 2,182 rows: plenty of line breaks, far
  too much markup, and `QTextDocument::setMarkdown()` alone took 2,676 ms on the
  thread that draws. The answer there is not to fold but to decline — the window
  is measured for its longest run of table rows before it reaches the view, and
  over the budget the file is shown as source with the reason said out loud and a
  `Show: Rendered` in the strip for a reader who wants the page anyway.

  What is still unhit is the other shape: a `.md` or an `.html` with **no line
  breaks in it at all** and no markup to divide it either, which would reach the
  layout as one enormous block that neither the fold nor a row count can see. A
  minified `.md` is not a thing a tool produces, and the answer if somebody does
  produce one is to divide the block, not to insert line breaks the renderer
  discards.

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
  it is checked in `tst_TransferTaskUnderFault`. A device outliving its task is
  covered too since MOLE-364: a stream holds its drive itself
  (`IFileSystem::sharedSelf()`), so a preview left open across an unmount is a
  stream reading from a drive nothing else refers to rather than a use-after-free.
  What is missing is the other half: nothing tells the user that the drive they
  just removed is still being written to. Left as it is until somebody hits it.
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
- **Compression asks ADR-0027's question too, and answers it more bluntly.** A
  member's header declares its length before a byte of it is read, so libarchive
  pads a short entry with zeros and cuts a long one off -- there is no "the file
  really did shrink" reading available, because the size is already in the
  stream. Any mismatch abandons the whole archive rather than closing one that
  cannot be trusted, which matters here more than anywhere: this is the operation
  that then offers to delete its own input. See MOLE-338.
- **A sync between two drives asks both of ADR-0027's questions now**, and it
  asks the second one differently from a transfer. MOLE-98 gave its copy loop the
  short-read guard; MOLE-337 gave it the arrival check, but per directory over
  what the run itself wrote rather than over a plan -- a sync's plan says what
  *should* change, and half of it is deletions, so weighing the plan would be
  weighing the wrong list. What is not checked either way is a file the sync
  decided not to touch: two trees that match by size and date are taken at their
  word, which is what makes a second run cheap and is the whole bargain of the
  compare mode you chose.

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

  **What reading the tree says, so the next person does not have to find it out
  again.** MOLE-124 asked for a Windows build or a written reason there is not one;
  `.github/workflows/windows.yml` is the job that would produce the first, and this
  is what is known before it has ever run.

  Nothing in `src/` that a Windows compiler would see includes an unguarded POSIX
  header. There are three places that include one at all: `Pty.cpp` puts everything
  behind `Q_OS_UNIX` and reports the terminal unavailable rather than failing to
  build, `SessionLog.cpp` guards its backtrace with `__unix__`, and
  `NfsFileSystem.cpp` is only added to its target when libnfs is found — which is a
  Unix library, so that file is never handed to MSVC. The warning flags were already
  written for both compilers. What had to change was two things in the test tree: a
  resident-memory probe that reads `/proc` and calls `sysconf`, now compiled away
  where it cannot work, and `tst_SessionLog`, which crashes a forked child in every
  case and is therefore registered on Unix only. A suite that cannot be built is what
  stops a whole tier, where a case that skips costs one line of output.

  **What only a run can settle**, and this is why the job exists rather than a
  paragraph of confidence: whether Qt's add-on modules install unattended, what MSVC
  makes of code no MSVC has read, how paths and encodings behave once `QFileInfo` is
  answering for a different filesystem, and which fixtures skip because they shell
  out to `zip`, `tar` or `gzip`. The job cannot be run from the machine it was
  written on, and it is `workflow_dispatch` only until it has passed once — a
  scheduled job that is red from the day it lands teaches everybody to ignore it.

  **macOS is the same question with a different obstacle, and reading it found
  one real fault.** `.github/workflows/macos.yml` is its job, built the same way
  and with the project's own Ninja preset, because clang and Ninja are what
  everybody here builds with. What reading settles: the only Apple-specific code in
  the tree is one branch of `HostPlatform.h`, `forkpty` is already not linked
  there, and the /proc probe that had to be guarded for Windows degrades on a Mac
  on its own -- `Q_OS_UNIX` is true there, the file is simply not present, and the
  case skips. What reading *found* is that the session log's crash backtrace was
  guarded on `__unix__`, which Apple's compiler does not define: `execinfo.h`,
  `backtrace()` and `backtrace_symbols_fd()` are all present on macOS, so a Mac had
  no backtrace for the want of a macro -- while the suite that asserts one is built
  there. It is `Q_OS_UNIX` now, which changes nothing on Linux and cannot be
  verified from here; that case on the first green run is the check.

  **What has run, and what has not.** Until 2026-08-24 no workflow in this
  repository had ever executed -- the branch was unpushed for a while, and the three
  that are not the release are `workflow_dispatch` only. *Second family* has now
  run once and passed: configure, build and the fast tier on Fedora 40, 130 tests
  green against Arrow 15.0.2, as an unprivileged account with no locale. Its weekly
  schedule therefore stays enabled; a red one would have been switched off, for the
  reason the Windows job is dispatch-only.

  **That job follows `fedora:latest` since MOLE-389, and a release transition is
  allowed to break it.** It sat on `fedora:40` until a year after that release left
  support, where a distribution whose libraries have stopped moving cannot give the
  job a new answer — so nothing was red, and nothing being red was the failure.
  **A red run there is a task and not a reason to pin the image back**: it means
  something this project builds against has changed, which is the whole of what the
  job is for. The first build on the moving tag found two of those at once — a
  plugin class name Qt 6.10 refuses, and a libnfs whose read and write swapped
  their arguments — neither of which can fail on a machine with Qt 6.4 and libnfs
  5. The same applies to the `.rpm`, built in the same moving tag: the
  package is for the release it was built on, and `README.md` says so rather than
  offering it to RHEL.

  **The Windows and macOS jobs have deliberately not been dispatched, and the
  reason is the code rather than the runners.** The author's call, in their words:
  the other systems are not ready at the level of the code to build the application
  correctly. So the two files are the means, ready for the day that changes, and
  running them now would produce a red badge that says something everybody already
  knows. What each of them settles when it is finally run is in the paragraphs
  above.

  **Signing and notarisation are out of scope, and that is a decision rather than
  an oversight.** An unsigned application downloaded from the internet is refused
  by Gatekeeper, and the two ways past it are an Apple developer account wired into
  the pipeline -- a recurring cost, and the author's call -- or instructions for
  opening it anyway. MOLE-125 chose the instructions, and they are worth writing
  when there is something to open: an artefact whose only description is "nobody
  has run this" does not become useful by being told how to get past Gatekeeper.

  **Neither job attaches anything, and the reason is not the build.** Both install
  nothing optional on purpose, so what they produce has no archive browsing, no
  network drives, no git band, no Parquet grid and no terminal panel — the last on
  Windows regardless, which is MOLE-38. Publishing that would answer a question
  nobody asked. An artefact worth downloading needs the optional libraries first —
  vcpkg on Windows, Homebrew on macOS, and libsmbclient is the one that has no
  comfortable answer on either — and then a self-contained bundle, `windeployqt` or
  `macdeployqt`. In that order, because a .zip of a build nobody has seen compile is
  not a release; it is a guess with a filename.

Tracked as issues rather than kept here: the full-screen window geometry (#31),
FTP staging (#34), WebDAV against a real server (#35), video preview (#37) and
the terminal on Windows (#38).
