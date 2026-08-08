# Done

Finished work, newest first, with a line on how each one was resolved. Anything
still outstanding lives in [TODO.md](TODO.md).

This is not a changelog for users — it is the record of what was asked for and
what the answer turned out to be, including the ones where the first answer was
wrong.

---

## F3 did nothing on a folder

`F3` previews the file under the cursor, `currentFile()` returns nothing for a
directory, so the action was disabled and the key did nothing at all — which is
indistinguishable from a key that is broken. On a folder it now opens it, the same
thing `Return` does, through the same `openRow()` the pane already uses.

Handled in the pane rather than in the action, deliberately. The menu entry says
"Preview this file" and stays disabled on a folder, because that is what it says it
does; it is the *key* that carries the second meaning, which is how function keys
have always worked in a commander.

The test asserts which of the two paths ran, since both are one keypress and easy to
confuse: on a folder the listing navigates and no tab appears, on a file a tab
appears and the listing stays put. It holds the pane pointer from the start, because
`pane()` asks the current tab for its pane and the current tab is a preview by the
end — the first version of the test dereferenced that null and took the whole binary
down with it.

## The menu had one heap called Tools

Eleven entries under one heading, of two entirely different kinds. *Preview this
file*, *Terminal here*, *Add to set* and *Index this folder* do something to the
files in front of you and hand you back to the listing. *Analyse folder*, *Find
duplicates*, *Bulk rename*, *Sync folders*, *Saved reports*, *Alerts* and
*Scheduled jobs* open a tab that is a tool you then work in. Read as one list they
are indistinguishable, so finding anything meant reading all of it.

`Tools` is now `Operations` and `Workflows`, and the deciding question is written
down: does the entry do something to the files in front of you, or hand you a tool
to work with? The tie-break for the ones that sound like both — if it needs a tab of
its own to be useful at all, it is a workflow. *Bulk rename* is a workflow even
though it acts on a selection, because what it opens is a tool with rules and a
preview. *Add to set* is an operation even though the sets view is a workflow,
because adding the selection to a set is one act, finished when it is done.

This is an extension point, not decoration: `Section` is what a plugin picks, so
leaving it as one bucket guaranteed plugins would keep filling the same bucket. The
names, the rule and the alternatives that lost are in
[ADR-0003](docs/adr/0003-menu-sections.md) — including why not `Tasks` (the
application already shows running tasks in a strip and the word would mean two
things), why not `Selection` (*Terminal here* acts on the folder, not a selection),
and why not `Actions` (every entry in a menu is an action).

`Section::Tools` is gone rather than deprecated, which breaks any out-of-tree plugin
that named it. That is deliberate: an alias would let a plugin keep dodging the
question this change exists to force. `docs/WRITING_PLUGINS.md` documents both
sections and the default is now `Workflows`, since a contributed feature tab is the
common case.

Two tests, at both levels. The registry one proves the sections come out in a fixed
order and that entries land where they asked to; the application one proves it for
the real eleven entries rather than for a registry fed by a test. What no test can
settle is whether a given entry was *filed* correctly — that is what the rule and
its worked examples are for.

## The F4 menu stopped answering the keyboard halfway along

Opening the menu with F4 worked, and so did stepping along the headings and opening
one with Right or Enter. Coming back out was where it ended: Left closed the
submenu and left the menu it came from without the keyboard, so the arrows did
nothing from then on and the only way forward was the mouse — which is the whole
thing F4 exists to avoid.

Measured rather than assumed, and it took three attempts to get right, each one
corrected by what the previous measurement said. Restoring the focus inside the
`closed` handler does nothing, because Qt moves the focus as part of closing the
popup and takes back anything claimed there; deferring with `Qt.callLater` does
work, but `closed` only fires once the exit transition has finished, which is
around a fifth of a second in which the menu is still deaf — long enough for the
next keystroke to fall into the gap, and long enough that an early version of the
fix reset the highlight *after* the user had already moved off it. The hand-back
now happens on `aboutToHide` as well, and it only restores the highlight when Qt
has actually cleared it, so it can never undo a heading the user has just moved to.

The five submenus were five near-identical blocks, and they had already drifted:
only File declared `focus: true`. They are one `SectionMenu` component now, which
is why the behaviour cannot differ between them again — the drift was part of the
bug, not tidiness. Each submenu's `objectName` follows its section name, so a test
can address one without a second thing to keep in step.

`f4MenuWalksIntoSubmenusWithTheKeyboard` walks the whole path — open, along,
in with Right, within, out with Left, in again with Enter, out with Escape — and
was checked against the bug by putting it back. The rule this belongs to is in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md), alongside the
terminal: focus declared is not focus held, and that applies coming back as much as
going in.

## The terminal did not get the keyboard, and Ctrl+D bookmarked instead of closing

Opening the panel left the keyboard on the file list, so the shell was on screen
while what you typed went somewhere else, and it took a click before it answered.
`focus: true` declares an intention, not a fact — something has to call
`forceActiveFocus()` when the panel is revealed, which is what the menu already
does when F4 opens it. The panel now does it too, on both paths: when it is
revealed after it exists, and when it exists only once it is already being
revealed.

`Ctrl+D` was the more interesting one. In a shell it means end of input, and the
encoding for it was already correct — `Ctrl+A`..`Ctrl+_` become control characters,
which is how `Ctrl+C` reaches the shell as an interrupt. The key simply never
arrived: it is a window `Shortcut` bound to `mole.bookmarks.add`, and Qt matches
shortcuts before offering the key to whatever has the keyboard, so the panel's own
handler — the one whose comment insists that every key goes to the shell — was
never consulted. The panel now accepts `ShortcutOverride` for everything, which is
Qt's own way for a focused item to say the key is its business.

A shell that ends should take its panel with it, which is what closing a terminal
means everywhere else, so a clean exit hides the panel. A shell that died of
something keeps it open along with the exit code, because otherwise the reason
disappears with the window.

This was the third collision of the same kind, after `F5` being swallowed by
`StandardKey.Refresh` and `Ctrl+W` being claimed by a read-only editor, so the rule
for all three is written down in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md) rather than
being rediscovered a fourth time. Note that the terminal needed the opposite of
`ViewerKeys`: not a view handing a shortcut back to the window, but a view taking
one away from it.

Three assertions, and each was checked against its own bug by putting the bug back.
Typing after the panel opens reaches the shell — typed on the keyboard, not sent
through the controller, because every other assertion about the shell would pass
with the keyboard on the list behind it. `Ctrl+D` ends the shell, closes the panel
and adds no bookmark. And `Ctrl+D` on the listing still bookmarks the folder, which
is where that behaviour belongs.

## A slow table preview looked like a hang

Opening a large CSV showed an empty grid until the whole file had been imported,
however long that took, while the task strip insisted something was running. The
view said nothing, and a view that says nothing reads as a frozen application.

The comment in `TablePreviewController::reimport()` claimed the opposite — *"Rows
appear as they arrive rather than after the whole file"* — and it was not true. The
progress handler called `TableModel::refresh()`, but the model's source was only
attached in the `finished` handler, and `refresh()` without a source reports no
headers and no rows. So every batch of five thousand rows refreshed a model that
had nothing to look at. The store is a database that answers for whatever has been
committed to it, so the source is now attached before the import is submitted, and
the finish refreshes rather than re-sourcing — re-sourcing would clear a filter
typed while the file was still being read.

Fixing that broke a test, which was the interesting part: `parsesCsvWithADetected
Separator` had been using "there are rows" as its signal for "the import has
finished", and the detected separator was only published at the end. With rows now
arriving early, the picker above a half-filled grid was captioned with the default
guess instead of the separator actually in use. The task announces the separator
the moment the shape is settled, before the first row is stored, so the caption
tells the truth from the first row on.

What is left is the gap before the first batch, which on a slow drive is the whole
problem. The view now says it is reading, after one second, in the middle of the
grid — the threshold and the wording follow the file pane, which had solved this
already for slow folders — and gets out of the way as soon as rows land, because
rows are a better answer to "is this stuck" than any spinner.

Both halves are covered. `tableFillsWhileTheImportIsStillRunning` samples the row
count from the progress signal rather than polling, because a poll that arrives one
turn late would be looking at the finished state and pass without ever seeing the
middle. The view half needed a drive that is genuinely slow to open a file, so
`MemoryFileSystem` grew `setReadDelayMs` beside the `setListDelayMs` that the slow
folder test already used.

## Markdown previews were cramped

Qt's Markdown importer gives a heading no space above or below it, sets every
paragraph solid, and hands a fenced code block to the view as nine-point
monospace with no margins and nothing behind it. Rendered, it read as a wall of
text, which is the opposite of what a Markdown file is for.

The document is now restyled after the import: headings get room and a size that
shows the hierarchy, prose gets line spacing, code gets the application's
monospace family at a size that matches the prose and a slab behind it, quotes
keep their nesting and go quieter, tables get cell padding. The view stopped
running the text edge to edge — it keeps margins, and on a wide window the
gutters take the surplus so the line length stays readable.

Two things had to be found out by measuring rather than by reading the
documentation, and both are now written down in
[ADR-0001](docs/adr/0001-markdown-preview-typography.md): a style sheet cannot do
any of this, because `setMarkdown()` never consults one; and wrapping a code
block in a padded frame — the only thing in Qt's rich text with real padding —
injects blank lines into the document and mangles what the file says, so the rule
is formats only, never structure.

Two bugs the tests caught before they could ship. A paragraph that merely opens
with an inline `code span` is given a monospace block font by the importer, so
detecting code blocks that way handed such a paragraph a slab of its own; only
unbreakable lines are a safe signal. And the styling read a quote's nesting depth
out of the very margin it had just overwritten, which flattened every nested
quote to one level — the depth is now recorded before it goes. Applying the
styling twice is a no-op, and a test asserts it, because it runs again on every
change the document makes, including its own.

The one thing left alone is the importer itself: a blockquote and a fenced code
block each end with a stray empty block, and a table placed straight after either
one takes that block into its first cell, which loses its bold. It happens before
any of this code runs, and correcting it would mean editing the document's
structure.

## Terminal panel

A shell for the folder you are looking at, split along the bottom of the window.
Opening it starts there; navigating afterwards does not drag it along, because a
shell has its own idea of where it is and fighting that is worse than leaving it
alone. `Ctrl+\`` opens and closes it, and every other key goes to the shell — a
terminal that let the window keep `Ctrl+C` would be useless.

libvterm does the emulation when it is available, which makes full-screen
programs work properly rather than approximately; the alternate screen is enabled
so leaving an editor restores what was underneath it. Without libvterm there is a
built-in parser covering printable text, the control characters a shell relies
on, cursor movement, erasing and colour — and it says "basic mode" in the header
rather than drawing something subtly wrong.

Two things the emulator has to get right that are easy to miss, and both are
tested: an escape sequence split across two reads, and a multi-byte character
split across two reads. A read boundary falls wherever the kernel puts it.

Not available on a virtual drive, and the panel says so — there is no directory
for a process to start in inside a zip or a bucket.

## Sync

A desktop rsync in its own tab, between any two drives, because everything goes
through the VFS.

Three modes, because everybody's idea of "sync" is different: **Update** copies
what is missing or changed and never deletes, **Mirror** makes the destination
match exactly including removals, **Fill gaps** only adds what is absent. Files
are judged changed by size and time, by size alone for drives whose timestamps
cannot be trusted, or by contents when certainty is worth the reading.

The dry run is the default and Preview is the prominent button. It is not a
simulation of the real path — it *is* the real path with the last step withheld,
which is the only way a preview is worth believing. A mirror that would delete
anything asks again before it does.

Details that only show up when someone relies on them, each with a test:
timestamps get a second of slack, or every sync between two filesystems copies
everything every time; a narrow include beats a broad exclude, because
"everything except .tmp, but definitely notes" is how people express it; a
filtered-out name is never deleted by a mirror, since acting on a rule the user
did not give is worse than leaving a stray file; directories are created before
the files that go in them and deletions come last, so a cancelled mirror cannot
lose something it was about to be handed back.

## Duplicate detection

Four strategies behind one interface, expressed as ordered *stages* rather than
one comparison — because that is the shape the problem has. Every worthwhile
strategy starts with something cheap that rules most files out and only then pays
for something expensive on what is left.

"Identical contents" is size, then a hash of the first 16 kB, then a hash of the
whole file. A test with a counting strategy proves the point directly: of ten
files, ten reach the size stage and two reach the reading stage. Hashing the tree
is the obvious approach and is the difference between minutes and hours on a NAS.

The other three exist because they answer questions content comparison cannot.
"Same name" finds copies that were edited apart — where else did this file end
up — which no hash will ever pair.

Choosing what to keep is the hard half, and the tab never picks for you. It
offers the choices people actually make — keep the newest, the oldest, the copy
nearest the top of the tree — and says what each would free before anything is
deleted. Empty files are ignored: every one is identical to every other, and
listing thousands buries the results that matter.

## Bulk rename

A list of independent operations applied in order, with a live preview of every
file's before-and-after. The preview is the feature: renaming two hundred files
on faith is how people lose an evening.

Eight operations to begin with — replace (plain or pattern), case, insert,
remove, strip a character class, number, affix, extension — and the order is
meaning, not decoration: stripping digits before numbering is a different result
from numbering first, and both are legitimate. A form with eight fields could not
express either.

It refuses a batch that would collide rather than discovering it halfway: two
files taking one name, a name already taken by something outside the batch, a
name reduced to nothing, a name containing a path separator, or a file left with
only an extension. The filesystem would only notice the second collision, by
which time the first file has already moved.

Rules touch the stem by default — upper-casing a name should not turn `.txt` into
something no tool recognises — and `.gitignore` is treated as a name with no
extension rather than as an extension with no name.

## File sets

A named list of files built by hand, from anywhere, across any number of drives,
then treated as a thing in its own right.

The whole design rests on one decision: a set answers `targetUris()`, the same
question a pane's selection answers, under the same name. So bulk rename,
analysis and the rest take a set with no code of their own — a test asserts
exactly that, because it is the property that would quietly rot first. The shell
asks the current tab what it is aimed at and never asks whether that tab is a set.

A set outlives the files in it, so it can be checked: "not looked at yet" and
"not there" are distinct states, because reporting a healthy set as broken before
anything had looked would be worse than saying nothing.

## SQLite and Parquet previews

Two more viewers, both landing on a grid, and neither of them importing anything.

A SQLite file is already a queryable table, so paging and filtering are queries
against the file itself — a database of any size opens at once. It is opened
read-only through SQLite's URI form, the only way it will refuse writes outright:
previewing a file is not a licence to modify it, and a database another process
has open is exactly where that goes wrong. `immutable` is deliberately not set,
because that would promise the file cannot change while another process may well
be writing to it.

Parquet is columnar and stores rows in groups, so a window only decodes the
groups it touches. Filtering it does mean scanning — there is no query engine
behind the format — so the scan is bounded and the view says so rather than
letting an incomplete count look authoritative.

The work was mostly not the readers. The grid was written against the CSV
importer, so it grew an `ITableSource` interface and moved into `DataGrid.qml`;
selection, copying, column sizing and the filter are now shared by all three
viewers rather than existing in three drifting copies.

Arrow is optional. When it is absent `ParquetTable::isSupported()` is false, the
provider declines the file and it falls through to the information viewer — a
missing optional library must never stop the application being built. Arrow's
headers have to be included before any Qt header, because Arrow declares a
parameter named `signals` and Qt's macro of that name expands to `public:`.

## Permissions of the current folder

Beside the report and index tags, what the current user may do here.

Modelled as questions — may I read, write, add files here, delete this — rather
than as mode bits, because POSIX mode bits do not describe a Windows ACL and
neither describes a bucket policy. `Unknown` is a first-class answer: a drive
with no idea says so and the interface shows nothing, rather than a guess
presented as fact. The native form is offered alongside where the platform has
one, which on Linux is the nine characters everybody reads.

Optional on `IFileSystem` like `space()`, and the conformance suite now checks
the contract from both sides: a backend advertising the capability must answer,
and one that does not must refuse rather than return something empty.

Removing an entry is governed by the parent directory, not the entry — a
read-only file in a writable folder can still be deleted, and reporting otherwise
would be wrong in the direction that matters.

## Tasks report whatever their work is about

Progress was counted in files, which is useless for the case a progress bar
exists for: one 4 GB file sat at 0% and then jumped to 100%. It is counted in
bytes now, with throughput measured over a short window rather than over the
whole run, so a stall shows up instead of being averaged away by a fast start.

More importantly the mechanism is general. A task publishes named metrics — a
key, a label, a value and a kind (count, bytes, rate, duration, text) — and the
strip lays out whatever it finds. Sync, duplicate detection and bulk rename will
each have something different worth watching, and none of them should require the
interface to learn new vocabulary. Bytes and speed are simply the first two
users, published through a convenience that also drives the percentage.

Every task also carries when it started and how long it has been going, frozen
once it ends. An elapsed time that keeps counting after the task finished is not
a measurement of anything.

A cancelled task no longer animates. Progress of -1 means "unknown", which is
right while running and wrong afterwards — a cancelled scan was left with a bar
sweeping for ever, as though the work were still going.

## Copy and move ask the right questions first

The confirmation now shows how much is going where, offers a different name for a
single item, and names the files that already exist at the destination *before*
anything happens. Collisions come from the listing the other pane has already
loaded, so the warning is on screen the instant the dialog opens — the only
moment it is useful.

The conflict choice is explicit: stop and report, skip that file, or overwrite.
Stop is the default, because a prompt whose safe answer is not the default is a
prompt that will one day overwrite something by reflex. Choosing overwrite says
plainly that there is no undo.

## Opening a report is not a rescan

`setTargets` always walked the tree, so looking at yesterday's numbers cost a
full scan — minutes on a large folder. It loads what is saved now and walks only
a folder that has nothing saved, because an empty tab would be useless. "Analyse
folder" is a separate method that always walks, which is what asking for it
means.

## Smaller things

- **Ctrl+G showed a clipped path field.** The crumbs and the editable path share
  one slot, and the slot had a hard-coded height of 30 while a Material text
  field wants 40 — so the field appeared with its text and underline cut off,
  which reads as being covered rather than as being too small. The slot is
  measured from the field now, and from the field rather than per mode, or the
  bar would change height as the keyboard moved into it.
- **The waiting view is centred.** Same fault as the empty window: a
  `ColumnLayout` is only as wide as its widest child, so the message sat against
  the left edge of the pane. Now tested against a drive that is genuinely slow,
  which also covers the one-second threshold for the first time.
- **The mouse no longer highlights rows.** Two highlights competed for one
  meaning — the cursor is where Enter will act, and a second one trailing the
  pointer made it ambiguous which row that was.

## F5 did not copy anything

Two causes, both of which the test suite was blind to.

The first was mine, from converting `canTransfer` to a property: `BrowserView`
still called it as a method in one place. Calling a bool as a function throws,
and the throw took the rest of the handler with it — so F5 opened no dialog,
reported no error and did nothing at all.

The second was older. `StandardKey.Refresh` is `F5` as well as `Ctrl+R` on this
platform, so the window shortcut consumed F5 before the pane ever saw it. Refresh
is `Ctrl+R` only now; F5 is the commander copy key and the window has no business
taking it.

Both survived a green suite because every existing test called
`copyToOtherPane()` on the controller. The walkthrough now presses F5, accepts
the real dialog and waits for the file to appear on the other side — the only
shape of test that could have caught either fault. Finding the dialog needed a
harness addition: a `Dialog` is a `Popup`, absent from the visual tree, and
`QObject::findChild` on the window finds nothing because QML does not parent
items into the window's QObject tree. `object()` searches both hierarchies.

## Background work you cannot miss

The strip reported a count, which read as decoration. While anything is running
it is now tinted, ruled in the accent colour, and carries the running task's
name, a real progress bar and its status line — collapsed, without expanding
anything.

Finished rows retire themselves after an hour. `Task` is stamped when it reaches
a terminal state and the manager sweeps every minute. A list nobody prunes grows
for the whole session, and by the end the one failure worth seeing is buried in
it. Work still running is never swept, however stale the list.

`Hidden` moved from the status line to the toolbar, beside copy and move: it
changes what the pane shows, which is what that strip is for. It applies to both
panes, so a dual view cannot hold two different ideas of what is in one tree.

## The browser toolbar says what is already known

`Copy` and `Move` never enabled in dual pane. They were bound to
`controller.canTransfer()` — an invokable, so there was no change signal and QML
evaluated the binding once. Switching to Dual satisfied the condition with
nothing to notice it. It is a `Q_PROPERTY` with a notify signal now, fed by every
input the answer depends on: the mode, the selection and whether the far side is
writable.

The `Index folder` button is gone; indexing is a once-in-a-while action and lives
in the menu. In its place the strip carries what the application already knows
about the folder: whether it has a report (clickable — it opens the saved one
rather than rescanning), whether an alert is watching it and whether that alert
has tripped, and whether it has been indexed and when.

The index is asked about the volume the folder sits *under*, not the folder
itself: scanning `/data` indexes `/data/projects` too, and claiming otherwise
would send the user to re-scan what is already there.

## Report and alert tags on the listing

The same facts per row, beside the date. Affordability was the whole design
question: a store lookup per row would make a listing of five thousand entries
pay thousands of file opens for two small tags. Instead the report store hands
over its stored folder names in one directory read, and each row is a hash and a
set lookup with no I/O at all.

## Reports library

A tab listing every saved report — folders on the left, that folder's runs on the
right, with what each run changed by against the one before it. Sorted by most
recent activity rather than by name, because a library sorted alphabetically
makes you hunt for the one thing that moved.

The store moved out of the analysis feature and into the host, alongside the
schedule and the alerts. Three things now need it — the library, the browser
strip and alerts reading the latest report — and a store owned by one tab is a
store the others cannot see.

## Clickable breadcrumbs

`/mnt/nas/projekty` is now `/ › mnt › nas › projekty`, each piece a target.
Pressing Backspace once per level was work the interface could do. Typing is
still there on Ctrl+G or a click past the last crumb, because a pasted path has
to go somewhere. A long path scrolls to keep the end in view rather than the
start, which you already know.

## Going back restores the cursor

Navigation left the cursor at the top of every listing, so walking a tree meant
restarting at each level. The pane now remembers where the cursor stood in each
folder, and stepping up lands on the folder just left. Bounded to a few hundred
folders: the convenience is not worth an entry per folder ever visited.

An entry that has since been deleted falls back to the first row — landing on a
stale index would be worse than landing on the top.

## Ctrl+W in a preview

Clicking into a preview stopped `Ctrl+W` from closing the tab. Measured rather
than guessed: a `Keys` handler on the text area showed the key arriving there,
which meant the read-only `TextArea` had accepted the shortcut-override event and
Qt had skipped the matching `Shortcut`. `Ctrl+W` is `DeleteStartOfWord` in the
standard editing bindings, and the control then discarded it because the document
is read-only.

Qt offers no declarative way to un-claim those keys, so `ViewerKeys.qml` hands
them back to the window — one relay rather than a private copy of the shortcut
table per view, and narrow enough to leave the keys a viewer genuinely uses.

Two further defects surfaced while doing it. `attachHighlighter` was called on
every text change, and attaching rehighlights, which changes the text: infinite
recursion, reported only as a stack-overflow warning nobody had read. And the
final window of a large file was unreachable — snapping the window's start back
to a line boundary shortened it, so "is there more after this?" was always yes.

## Alerts

A tab that lists what is being watched, what tripped, and a form for watching
one more. Eleven metrics — total size, free space in bytes and per cent, file
and folder counts, largest file, hours since anything changed, permissions, last
modified, existence, unreadable folders — compared with above / below / changed
/ equals.

Two design points worth keeping:

- A metric that could not be read is `Failed`, never `Ok`. An unreachable drive
  reported as a green tick is the worst outcome available, because it looks
  exactly like everything being fine.
- `Changed` treats its first reading as a baseline rather than firing. Otherwise
  every alert would trip the moment it was created, which teaches the user to
  ignore the first one they ever set.

An alert can read from the latest saved report instead of measuring live —
instant, and only as fresh as that report, which is what scheduling the report
is for. It deliberately does not fall back to a live walk when no report exists;
that would quietly turn it into a different alert with different timing.

## Delimited files with no row limit

The CSV/TSV viewer stopped at 5000 rows and filtered only what it had loaded.
Now the file is streamed into a scratch SQLite database and every question is a
query, so paging and filtering cover the whole file however large it is.

- `DelimitedStreamParser` parses a chunk at a time and carries the state a row
  straddling a chunk boundary needs, including a quoted field with newlines in
  it.
- Columns are measured from the contents during the import, so the grid fits
  what is in it instead of a default that wasted half the window.
- Cells select by click, shift-click and drag, and copy as tab-separated text —
  what every spreadsheet expects on paste.
- The table widens to the widest row in the sample rather than to the header
  alone: a header that does not mention every column is common, and sizing to it
  would silently drop the extra fields.

## Text preview of very large files

The viewer held the whole file. Now it holds only the window being shown, read
through a seek, so a 100 GB log opens as fast as a 100 byte one. Windows snap to
line boundaries at both ends, or paging would show a severed line at every step.

Alongside it: source highlighting for twenty-odd languages from a table rather
than hand-written code per language, Markdown rendered instead of coloured, and
one monospace family chosen once by the application so every code and data view
lines up.

## Drive capacity in the sidebar

Each drive shows how full it is — amber past three quarters, red past nine
tenths — with free and total beside it. Drives that have no meaningful capacity
show only a name: a bucket has no size in any useful sense, and a chart is read
as a fact.

`IFileSystem::space()` is optional and defaults to `NotSupported`; only backends
advertising `ReportsSpace` are asked. The query goes through a task like
everything else that touches storage, because `QStorageInfo` blocks on an
unreachable NFS mount and the UI thread must never wait on a disk.

## Automation

Reports can be put on a clock. `Scheduler` polls rather than arming a timer per
rule, because a laptop asleep for two days has to notice on waking. A rule that
has never run is due immediately, so a job whose turn came while the application
was closed runs at the next start instead of waiting out another interval.

The tracking tab sorts broken rules first, counts consecutive failures, and
records three things the design deliberately makes visible: a rule whose plugin
is gone is `Skipped` with the reason, a run interrupted by a quit comes back as
`Failed` rather than stuck at `Running`, and every attempt is logged whether or
not it fired.

## A test harness that does not lie

The Xvfb-and-`xdotool` setup was worse than no harness: with no window manager
there is no X input focus, so roughly two of every six synthetic keystrokes
arrived and tests went green because nothing happened. It cost four rounds on
one Enter-key bug — a real defect that looked like a harness fault, next to a
harness fault that looked like a real defect.

`QmlAppHarness` builds the whole application offscreen and posts keys straight
to the `QQuickWindow`: the same delivery path production uses, minus the
display. Screenshots come from `QQuickWindow::grabWindow()`, so `make
screenshots` cannot produce a picture of a state the assertions did not just
verify.

## Smaller things

- **Closing a tab returns to the one it was opened from.** Each tab remembers
  its opener; position alone would send you to whichever tab happened to sit
  next to it, somewhere you were never working.
- **A working tab says so.** `FeatureController::busy` now reaches the tab strip
  as a spinner, so a report still running on a large tree does not look like one
  that finished.
- **The filter keeps the keyboard while narrowing.** Enter opened nothing and an
  arrow moved focus instead of the cursor; both cost a keystroke and swallowed
  the one just spent. Fixing it surfaced a second defect: navigating with a
  filter active left the new folder silently filtered by the old term, with
  nothing on screen to explain the missing files.
- **The empty window is centred.** A `ColumnLayout` is only as wide as its
  widest child, so centring inside it put the block off to one side of a wide
  window; and the tab stack kept claiming half the height until it was hidden
  rather than merely emptied.
- **The empty window offers only what it can open.** `IFeature::needsContext()`
  marks a tab that is meaningless without a selection — a preview needs a file,
  a report needs a folder — and those are left out. A button that opens an empty
  tab reads as broken rather than as inapplicable.
- **Combo boxes size to their widest entry.** The repeat picker had a fixed
  width and truncated its own labels.

## Adding a drive did nothing

Reported: pressing "+" under "Your drives" had no visible effect. Three separate
faults sat on top of each other, and each on its own was enough to produce
exactly that symptom.

**The form had no size.** The field area was a `ScrollView` whose content was
sized to the view. A `ScrollView` takes its own implicit size from its content,
so that closes a loop: width decides whether a scrollbar is needed, the
scrollbar decides the available width, the available width decides the width.
Qt detects the loop, prints `Polish loop detected. Aborting after two
iterations.` and abandons the layout — which leaves every child present,
visible in the tree, and zero pixels tall. It is now a `Flickable`, which takes
its size from the layout that placed it and is told its content size, so
nothing feeds back.

**The list could not update.** `model: App.configuredDrives()` and
`model: App.driveKinds()` bound to method calls. A method call in a QML binding
has no change signal, so it is evaluated once and never again — a saved drive
would never appear in the list beside the form however well the save worked.
Both are now `Q_PROPERTY` with `NOTIFY drivesChanged`, emitted from save,
remove, connect, disconnect and unlock. This is the third time this session
this exact defect has appeared, after `canTransfer()` and `rowSpans()`.

**Pressing add changed nothing.** The dialog already opened in the blank
"new drive" state, so a button that reset it left the screen exactly as it was.
`beginAdding()` now opens the kind picker as well, and the empty right-hand
panel says what to do instead of sitting blank. The reset stayed a separate
function because saving also resets, and a save should not throw a
sixty-backend dropdown open in the user's face.

Also capped that dropdown's height: at full size sixty entries covered the
dialog, the window behind it and the drive list being chosen for.

The lesson is the one already recorded above under the F5 bug, restated: every
existing drives test called `saveDrive()` on the controller, and the controller
was fine. Only a test that presses the button can see a button that does
nothing. `theDrivesDialogOffersBackendsAndAForm` now clicks "+", picks a kind,
asserts the form is on screen *with a size*, saves, and waits for the drive to
appear in the list.

## A log for the runs that end badly

`make run` now keeps a session log, because a crash takes the terminal's
scrollback with it and the useful lines are the ones printed just before the
fall.

Every line is flushed to the file and then to the operating system as it is
written. A buffered log loses exactly the part worth reading. The previous run
is kept beside the current one as `session.log.1`, because the way anybody
reacts to a crash is to start the program again, and that restart is what would
otherwise destroy the evidence.

When the program does fall over, a signal handler writes the backtrace into the
same file and then lets the process die exactly as it would have -- default
handler restored, signal re-raised -- so the shell still reports a crash and any
core pattern still gets its core. The handler allocates nothing, locks nothing
and calls no Qt: all three deadlock or fault a second time in a process that is
already broken. It writes with `write()` and `backtrace_symbols_fd()` to a
descriptor captured when logging started.

The tests crash on purpose, in a forked child, and the parent reads what the
child managed to write on its way down. That covers the part that is easy to
get wrong and impossible to notice: `tst_SessionLog` caught the signal number
being written as "06" instead of "6".

## Two type errors in the drives form

Reported alongside the above: the form appeared but printed
`Unable to assign QJSValue to QString` once per field.

Field defaults come from each backend's own metadata, so they arrive as
numbers, booleans, nulls and lists as well as text -- and a text field handed a
list refuses the whole binding, leaving the field blank. `fieldValue()` now
returns a string whatever it is given. A second warning with the same root,
`Could not convert array value at position 0 from QString to QChar`, went with
it: that is what assigning a JS array to a string property looks like.

The test that covers it builds a form for every one of the 59 backends and
fails on any warning at all, rather than leaving them to be noticed in a
console. 707 fields, silent.

## The segmentation fault: a layout re-entering itself

Reproduced, with a stack, and fixed.

The log's own backtrace stopped after two frames -- rclone brings a Go runtime
into the process, Go forwards signals to handlers installed before it started,
and no in-process unwinder can walk past that trampoline. So the application was
driven under gdb on a virtual X server with xdotool, through the reported steps:
open Drives, set a passphrase, then switch backends in the dropdown. It fell over
on the fifth switch, and gdb had the whole stack:

    qmlAttachedPropertiesObject
    QGridLayoutItem::stretchFactor
    QGridLayoutEngine::setGeometries
    QQuickGridLayoutBase::rearrange
    QQuickLayout::geometryChange
    QQuickItem::setWidth
    QQuickFlickable::geometryChange      <- the scroller
    QQuickGridLayoutBase::rearrange      <- already rearranging
    QQuickLayout::updatePolish

A layout inside a scroller, with its width bound back to that scroller,
re-enters itself: the outer layout sizes the scroller, the column's width
binding fires, and the column rearranges while the outer rearrange is still on
the stack. Switching backends destroys every field while that is happening, and
the layout engine reads the attached `Layout` properties of an item already on
its way out. That read is the crash.

It is the same circularity that produced `Polish loop detected` earlier in this
file. Fixing the warning did not fix the loop -- it moved it somewhere Qt could
not detect it, where instead of giving up it recursed until it touched a dead
object. The fields are now a `ListView`: a view owns its delegates, expects its
model to change under it, and never drives the layout that placed it.

Two things the harness could not see, and one it now can. It could not see the
crash: offscreen and even on a real X server, driving the same properties from
C++ never produced the polish cycle that a rendered window does. It could not
see that the dropdown had stopped opening at all -- capping `popup.height`
against `popup.implicitHeight` collapsed it to nothing, because that height is
zero while the popup is shut. That one is now a test: open the dropdown and
assert it is between 100 and 340 pixels tall, since "opens" and "is visible" are
not the same claim. The form test also now asserts the fields have a width,
which is what a layout the engine has abandoned does not give them.
