# Done

Finished work, newest first, with a line on how each one was resolved. Anything
still outstanding lives in [TODO.md](TODO.md).

This is not a changelog for users — it is the record of what was asked for and
what the answer turned out to be, including the ones where the first answer was
wrong.

---

## Search results were a list you could only look at

Three things they could not do, and one of them was in the wrong place.

Building a set from them existed, beside the criteria — which is not where the rows
are. It has moved to a strip above the results, along with the two other things worth
doing to a match: showing it in its folder, and looking at it without leaving. The
index search shows the same strip without the set button, since it has nowhere to put
one yet.

Walking them was impossible: the list had no focus handling at all, so results could be
read and double-clicked and nothing else. Arrows move now, Enter goes there, F3 previews,
and Down out of the query box walks into the answers — once a search has answered, the
answers are where the keyboard should be. Arriving at a fresh list puts the cursor on the
first row rather than nowhere, which took a second attempt: results arrive after the view
exists, and a model that had no rows leaves `currentIndex` at -1.

"Show me where this is" is the natural end of most searches and had no way to be asked
for. `revealFile()` opens the folder holding a file *with the cursor on the file* —
arriving in the right folder with the cursor somewhere else is only half an answer. The
listing lands asynchronously, so the pane remembers what it was asked to reveal and
consumes it when the entries arrive; a file already in the current folder needs no
navigation and is selected straight away. Both paths are tested, because the second is
the one that quietly does nothing if forgotten.

## Previews had no options, and nowhere to remember one

An `.html` file previewed as coloured source. Sometimes that is what someone wants and
sometimes they want to read the page, which makes it a setting — and there was nowhere
to put a setting: `SessionStore` remembered which tabs were open and the window
geometry, and nothing else about anything.

Three answers, all in [ADR-0006](docs/adr/0006-preview-options-and-preferences.md).
`Preferences` is one small file of dotted keys that knows nothing about what they mean.
A provider *declares* its options — key, title, choices, default — and the strip above
the preview renders them without knowing what any of them are, the same way the menu
renders entries from plugins it has never heard of. And a choice is keyed by provider
*and* suffix, because "the next `.html`" is what was asked for and one text viewer
serves `.html`, `.xml` and `.svg` with different sensible answers; the provider id keeps
two viewers claiming a suffix from overwriting each other.

The choice applies immediately and is remembered, and it is applied *before* the file
is read, so opening the next `.html` shows the page straight away rather than showing
source and then correcting itself.

The rule that mattered most is the one about the network. Qt's rich text engine
resolves what a document names, so a page could quietly tell whoever wrote it that a
file had been looked at — in a file manager that is a nasty surprise, not a feature.
Anything a document could reach out with is removed before it is rendered: images,
scripts, stylesheets, frames, embedded objects, event handlers. Blunt rather than
clever, because telling a local reference from a remote one means parsing and resolving
and getting that subtly wrong is exactly the failure being prevented. The test feeds it
a deliberately hostile page and asserts that not one `http` survives while the words do.

Two mistakes worth recording. The first is mine and it leaked: adding a store without
teaching `PrivateProfile` about it meant the tests wrote into
`~/.local/share/Mole/mole-tests/preferences.json` — real user data, outside the sandbox
— which is why one test passed alone and failed in the suite, reading back what a
previous *run* had left. `MOLE_PREFERENCES_PATH` is in the profile's list now, the
leaked file is gone, and the suite was run twice to prove it. Adding a store means
teaching the test profile about it, or the tests are not isolated at all.

The second was the same delegate-recreation trap as bulk rename: republishing the option
list rebuilds the Repeater's delegate, so the test's pointer to the picker was dangling
and reading it hung the run. The test looks the item up again instead.

## A long search froze the interface

Reported as: a search that runs for a while eats so much CPU that the window stops
responding — and, tellingly, a folder analysis running alongside it took *longer*
and did nothing of the kind.

That comparison was the clue that mattered, because it ruled out the walk. The
analysis walks the same trees through the same `DirectoryWalker` and reports its
status just as often. What it never does is put anything into a model the interface
is watching.

`FileListModel::appendEntries()` reset the model and called `rebuildVisible()`,
which copies every entry found so far, filters it, and `stable_sort`s the lot. On
every batch. A search returning forty thousand results in batches of two hundred
therefore sorted a growing list two hundred times, on the thread that draws the
window. Measured before touching anything: **9,670 ms** of pure CPU for that case.

The batch is now filtered and sorted on its own and `std::inplace_merge`d into what
is already in order — both halves share the comparator, so the result is sorted
without looking at the earlier entries again. The same case now takes **246 ms**,
which is thirty-nine times less work in front of the person waiting.

Worth saying plainly: the time-based flushing added an hour earlier made this worse
in exactly the case reported. Before it, batches only went out every two hundred
matches; after it, also every hundred and twenty milliseconds — so a long search
produced more batches, and each batch cost a full re-sort. The latency fix was right
and the quadratic append underneath it was the bug; together they were the freeze.

The test states the case rather than the mechanism — forty thousand results in two
hundred batches, and a ceiling generous enough for a debug build on any machine —
and it fails with the number in the message, which is how the 9,670 ms above was
measured in the first place.

One thing deliberately left: a reset still discards the view's scroll position and
selection, so scrolling through results while they arrive is unsatisfying. Fixing that
means proper insert semantics rather than a reset, which is a bigger change than the
freeze warranted; it is recorded in TODO.md.

## Search results arrived late, and led nowhere

Two halves, and the first turned out to be one number.

`LiveSearchTask` batched matches at two hundred before emitting them — and only at
two hundred. A search over a large tree that matched a dozen files therefore showed
nothing at all until the whole walk had finished, which is exactly the case anyone
searching a disk meets. Batches now go out on whichever comes first, enough matches or
a hundred and twenty milliseconds, so the first answers arrive almost immediately and a
flood still costs one signal per two hundred rather than one per file.

The test for that was the interesting part. The suite already had
`streamsResultsWhileRunning`, which only checked the totals once everything had
finished — it proved nothing about arriving early, and it passed before and after. The
first replacement was no better: asserting a batch arrived while `isFinished()` was
false passes even with count-only batching, because the last flush happens inside
`run()` before the task is marked finished. Only a clock can answer *when*, so the test
now times the first batch against the whole walk and fails with a sentence that says
what went wrong: *first matches arrived at 1511 ms of a 1511 ms walk*.

The second half is what results are for. They can be narrowed where they are —
straight onto the model that already holds them, so no walk and no query, just less of
what is there — with a count that reads "3 of 41" when a filter is on. And they can
become a file set, which is where the work carries on: a snapshot of what is on screen,
narrowing included, because the rows in front of someone are what "these results"
means. A set that re-ran the query later would be a different promise from the one the
button makes. Nothing to build from produces no set rather than an empty one, and an
unnamed set is named after the query rather than after nothing.

## Ctrl+F was not usable as a search box

Three things, and only the last is a feature.

The keyboard was not in the field, so the first thing anyone did after pressing
`Ctrl+F` was reach for the mouse to click into it — which is exactly what the key is
supposed to save. It is focused now, on creation and whenever the shell asks the tab
for its pane.

Enter already started a search; nothing said one was *running*. A tree walk over a
large disk takes long enough that silence reads as nothing having happened, so the
results area now says it is searching while there is nothing to show, and steps aside
the moment rows arrive — the same threshold-free rule the table preview uses, because
here the walk streams matches from the start.

Then the criteria. Size, typed the way people write it: `10M`, `1.5 GiB`, `500k`,
`1,5M` with a comma, because that is a decimal point in most of Europe and this
application already shows sizes that way. Nothing and nonsense both mean "no limit"
rather than zero — a limit of zero bytes would quietly match nothing. It lives in a
*More* section that is folded away, so the common case stays one field and one key.

The interesting part was the index, and it needed a decision rather than code:
[ADR-0005](docs/adr/0005-which-engine-answers-a-search.md). The form now asks the
index when an indexed volume's root is a prefix of the folder being searched, and
walks otherwise. Partial coverage counts as none — the temptation was to ask the index
for the part it covers and walk the rest, which would produce one list where some rows
are current and some are as old as the last scan, with nothing to say which. The
toggle is on by default because the index is enormously faster and usually right, and
what makes that default safe is that the status line always names the engine that
answered and how old the index is. Turning it off is the case that matters: the truth
on disk right now, whatever the index remembers.

Both engines already had `minSize`/`maxSize`, which is why size was the criterion
added first — anything the index cannot express would have to fall back to walking and
say so.

Tested at both levels: the size parser on its own including the cases that must mean
"no limit", the engine choice as behaviour (unindexed walks, indexed answers from the
index, the toggle forces a walk on a file written after the scan), and the box itself
in the real window — the field holds the keyboard on opening, five typed characters
reach the controller, and a 500M floor empties a fixture that has nothing that big.

## A dist/ from before the rename failed the licence check

`make licence-check` had been failing on *bundled Qt cannot be replaced by the user*
since before any of the recent work, and it had nothing to do with licensing. A
`dist/` sat in the tree from a `make bundle` run made when the binary was still called
`superfilemanager`; the check looked for `dist/mole`, found nothing, and reported the
bundle as non-replaceable. Everything that actually mattered passed throughout — Qt
dynamically linked, no Qt symbols in the binary, no GPL-only module.

Seventy-six megabytes of git-ignored build output holding the old binary and copied
system libraries, with nothing hand-made in it. `make bundle` already begins with
`rm -rf dist`, so it was a stale artefact rather than a bug in the target, and deleting
it was the fix. The tree is left without a bundle, which is how a fresh checkout looks:
one is built on demand, and a bundle left lying about is precisely what caused this.

The change worth keeping is the message. Ten minutes went into working out what
"bundled Qt cannot be replaced" meant, so the check now names the launcher it wants —
*there is no launcher at dist/mole (stale bundle? run: make bundle)* — and separates
the three ways replaceability can fail instead of reporting one verdict for all of
them. A fresh `make bundle` was run end to end to confirm the check still passes when
there is something real to check, including the replaceability test that had never
actually run.

## The header says the palette is there

A shortcut nobody has been told about is a feature nobody has. So the title bar now
carries something that looks like the box it opens — a search glyph, the words *Search
commands*, and `Ctrl+R` drawn as a key — in the middle of the window, on the same line
as the hamburger and the name.

It opens the palette rather than trying to be one. Two boxes that both filter would
mean two places owning the same state, and the reason the palette works is that one
place owns the list.

Centred in the *window*, which took a second attempt: laid out in the toolbar's row
between the menu on one side and the task indicator on the other, it sat noticeably
right of centre, because equal spacers centre a thing between its neighbours and not
in the window. It is anchored to the toolbar instead. The test holds all three claims
that make it work as a teaching aid — visible, within two pixels of the window's
middle, on the same line as the menu button — and that clicking it opens the palette.

## The palette moved to Ctrl+R, and stopped remembering the last query

Three small things, and one of them was only found by trying to break the test.

The box kept whatever was typed into it last: `onAboutToShow` reset the model's
filter but not the field, so the next opening showed a list narrowed by a query the
user could no longer see a reason for. It clears the field now.

`Ctrl+R` belonged to Refresh, which was the wrong use of a key that good — refreshing
is one row in the palette like everything else, and it keeps its View menu entry. Its
`shortcut` label went with the binding, because a menu that advertises a key that no
longer works is worse than one that advertises nothing.

And the palette lost its animations. That started as a test problem — pressing the key
again straight after Escape did nothing, because `opened` goes false when the exit
transition *starts* and `open()` during that transition is silently ignored — but it is
a real one: a human closing and reopening quickly would hit exactly the same wall. A
box you summon to type one word into should be there the instant you ask.

The test was worth more than the fixes. Removing `field.clear()` and running it again
showed it still passing, which meant the assertions I thought I had written were not in
the file at all — the edit had not matched. Written properly, and checked the same way,
it now fails with `"termi"` still sitting in the box. It also has to check the clearing
*before* running a command, because the command it runs is the terminal, and a terminal
that holds the keyboard stops `Ctrl+R` reaching the window — which is ADR-0002 working
exactly as intended, in a place I had not expected to meet it.

## One input that can reach everything

`Ctrl+Shift+P` opens a box with a list underneath of everything that can be done
right now — the whole `F4` menu tree, every bookmark, every drive. Typing `termi`
leaves one row, *Operations → Terminal here*, and Enter runs it. Arrows move, Escape
leaves, and nothing about it needs the mouse, which matters because the reason it
exists is that not every control has a shortcut of its own.

The design decision that makes it worth trusting is that it holds no list. The menu
entries come from `ActionRegistry::buildModel()`, the places from `BookmarkModel` and
`MountListModel`; the palette is a view over those three. A second list maintained by
hand would drift out of step with the menu the first time somebody added an action,
and then the one thing the palette promises — that it has everything — would quietly
stop being true. The test that says so is the first one in the file: the palette's
paths must equal what the menu would show, entry for entry.

"Only what is available" came for free rather than needing a mechanism: the menu
already evaluates each entry's `enabled` callback at the moment it is asked, so a
greyed-out action is simply absent. It is rebuilt on every open, because what can be
done depends on the tab in front of the user.

Ranking is less optional than it looks. A title match beats a match on the group, or
typing `set` buries *Add to set* under everything in a section whose name contains
those letters; and several words match anywhere in the path in any order, so both
`op term` and `term op` find the terminal. Each of those is a test, because each is
a way for the box to feel broken while technically working.

The model asks and the shell acts — `actionRequested` and `locationRequested` rather
than a call into tabs or navigation — which is what lets it stay a plain view. The
walkthrough proves the whole path in the real window: the key opens it, the input has
the keyboard immediately, five characters narrow everything to one row, and Enter
opens the terminal.

## PDFs had no preview

A PDF fell through to the information viewer — the last resort for a file we cannot
show — so previewing one gave its size and its type and nothing of its contents,
while the listing already drew it an icon and `IPreviewProvider.h` named PDF in its
own description of what previewing is for.

It opens as a column of pages now, rendered by `QPdfDocument`, read-only, with the
same `Ctrl+PgUp`/`PgDn` paging the text viewer uses. Pages are rendered when a
delegate asks for one, so opening a six-hundred-page scan costs the first page rather
than six hundred, and the delegate reserves its height from the page's own aspect
first so the list does not jump about as images arrive. The rendered width is
quantised in steps because it goes into the cached file's name — bound to the raw
width, dragging a window would have re-rendered every visible page per pixel.

Two decisions were made before any code, and both are in
[ADR-0004](docs/adr/0004-pdf-previews.md). Qt PDF rather than poppler, because
poppler is GPL and does not sit with shipping Mole under Apache-2.0, while the Qt
module as packaged declares `LGPL-3 or GPL-2` — which is what the licence audit turns
on. And pages reach the screen as image files in a scratch directory rather than
through a `QQuickImageProvider`, because an image provider is registered on the
`QQmlEngine`, which a preview provider deliberately cannot reach; threading the engine
through the plugin boundary to save a temporary file would have traded a real
architectural rule for a smaller one.

`QtQuick.Pdf` would have supplied most of this view for free and was not used: its
QML module is not installed here, so depending on it would mean a second optional
dependency for one feature and a view that silently does not exist without it.
Rendering through `QPdfDocument` costs a page-image path and buys control over when
pages are rendered, which is the part that matters.

The dependency is optional. Without `Qt6::Pdf` the provider still compiles and still
refuses every file, so a PDF behaves exactly as it did before — and the test states
that both ways round, so a build without the module is a green build rather than a
skipped one.

Licence work done rather than promised: `THIRD-PARTY-NOTICES.md` records Qt Pdf and
what it embeds — PDFium and PDFium's own third-party components, all inside
`libQt6Pdf.so` rather than in Mole's binary — and the audit table in
`docs/LICENSING.md` lists the module. `make licence-check` confirms Qt is still
dynamically linked, now across twelve libraries including `libQt6Pdf.so`, and that no
GPL-only module is referenced. It also fails one check, on a stale `dist/` from an old
bundle whose launcher is still named `superfilemanager` — that failure predates this
work and is noted in TODO.md rather than quietly worked around.

The tests write their own PDF with `QPdfWriter`, because a binary fixture in the tree
is one nobody can review. They check the page count, that an A4 page comes out
upright, that asking twice at one width reuses the file while a different width
renders again, that a page past the end is nothing rather than a crash — and that the
rendered page has ink on it, since a renderer quietly producing white paper would pass
every other assertion. The walkthrough then proves the whole path in the real window:
a delegate asks, an image loads, and the strip says "Page 1 of 2".

## Bulk rename hid the thing it calls its own feature

`BulkRenameView.qml` opens by stating its own priority — *"The preview is the
feature"* — and then laid itself out as though the form were. The rules column asked
for 40% of the window and there was no minimum width anywhere in the view, so the
grids of full-width text boxes inside it stretched a two-character prefix across a
third of the screen and the before-and-after list took what was left. The form is
now capped, and the preview keeps a floor of its own, so no arrangement of rules can
crowd it out.

The other half was that nothing happened while you typed: the fields were wired to
`onEditingFinished`, so a prefix showed no effect until Enter was pressed or the
focus moved elsewhere — while the dropdowns and spin boxes in the same form updated
at once. Two behaviours in one panel, and the fields carrying the interesting part
were the ones that felt dead.

The first attempt at this was wrong and worth recording. Measuring `RenamePlan::build`
first — 2 ms for a thousand files, 15 ms for five thousand, 63 ms for twenty thousand
— it looked like live updates needed coalescing, so a debounce went in. That was
solving a problem nobody had: the complaint was about not seeing the changes, and a
debounce delays exactly the feedback being asked for. It also made the preview
asynchronous, which broke a test that reasonably expected the plan to be current. It
came out again.

The real cause was elsewhere and would have survived any amount of debouncing. Every
keystroke made `setRuleField` emit `rulesChanged()`, the form's `Repeater` rebuilt its
delegates, and the field being typed into was destroyed and replaced. Typing "2024_"
left "2": the first character round-tripped through the model, the field was
recreated, and the rest went nowhere. `setRuleField` no longer announces that the
rules changed — the form is the only thing that reads them and it is where the change
came from; what has to follow the keystroke is the preview, and `previewChanged()`
says so.

The test types into the field rather than calling the controller, and keeps the
keyboard there while it asserts, because the whole bug lived in the difference
between those two things. It also holds the layout: the preview list must keep at
least 320 pixels.

## The small controls were too small to hit

Adding a bookmark and closing a tab — the two things anyone does most — were
`ToolButton`s of 22 by 22 with a text glyph inside, and the drive's remove button was
20 by 20. Fiddly to hit, and they read as afterthoughts.

It was never two files. Fifty-two explicit `implicitWidth` or `implicitHeight` values
sat below 24 across 18 QML files, so the fix was a decision rather than a nudge:
`App.minimumTarget` is now the floor for anything that is only an icon, at 28 —
twenty-four is the figure usually quoted as a minimum for a pointer, and on a desktop
something nearer thirty stops feeling like a pinprick. Twenty-four controls were
raised to it, and nineteen glyphs now take their size from the type scale, because a
bigger button with the style's default mark in the middle looks emptier rather than
clearer.

What was left alone, deliberately. The remaining small sizes all belong to
`BusyIndicator`s, which are not click targets — a spinner does not need to be
reachable. And several of these controls appear only on hover, the drive's × among
them; that is a discoverability question rather than a size one, a drive can also be
removed from the Drives dialog, and changing when a control appears is a different
decision from how big it is when it does.

The floor is testable and the look is not, so the test holds the floor: the two
controls the request named are at least `minimumTarget` in both directions and their
glyph reports exactly `textSize`. It is two rather than all of them because a
tree-wide assertion would need every icon-only control to carry an `objectName`
first — recorded in TODO.md rather than left implied.

## The type was too small, and there was no scale to raise

A file name was 13 pixels, most of a listing 12, supporting text 11, and in places
it dropped to 10 and 9. A file manager is a thing people stare at all day.

Raising the numbers where they stood was the wrong shape of fix: there were about
270 `font.pixelSize` literals across 27 QML files, so "a bit bigger" would have been
270 edits and the next view added would have guessed its own size again. The sizes
now come from `AppController`, for the same reason the monospace family already did
— picked once so that a listing, a preview and a form line up instead of each
choosing. Five steps, each with a job: `headingSize`, `textSize` for primary content,
`secondaryTextSize` for sizes and dates and labels, `smallTextSize` as captions *and*
the floor, and `monospaceSize` for code, which reads a shade smaller than prose.
`listRowHeight` is derived from `textSize` rather than stated, so raising the text
cannot crop a row.

Applied where the reading happens: the file listing, every preview, and the sidebar
— which was not in the original plan but ended up looking small next to a listing
that had grown, and it is the next place the eye goes. Around 200 literals remain in
the other views and adopt the scale as those views are touched; that is recorded in
TODO.md rather than left as a surprise.

Constant for now, and deliberately so: when these become a preference the views do
not change, which is the whole point of them living in one place.

Two tests, because "looks better" is not assertable but two things around it are.
The scale's shape is held at the application level — the steps in order, nothing
below the floor of eleven, code no larger than prose — and the binding is held in the
real window: a listing row's name label must report exactly `textSize`, because a
literal left behind in a delegate is invisible until someone compares two views side
by side. Whether the result is *pleasant* is still what `make screenshots` and a
human are for.

## How big is this folder, answered in the listing

`Ctrl+Shift+S` measures the ticked folders — or every folder in the listing when
nothing is ticked, because "which of these is the big one" is the question — and
writes each total into its row as the walk finishes it. A background `Task` like
everything else that takes time: progress, cancellable, visible in the task strip,
and the window stays usable throughout.

No second tree-walker: `FolderSizesTask` uses the same `DirectoryWalker` the
analysis and the indexer use, so cancellation, unreadable directories and symlink
loops stay solved in one place. What it does not reuse is `AnalyseDirectoryTask`
itself, which was the first plan — it produces a whole `AnalysisReport` per folder,
and forty folders would mean forty reports built and thrown away to read one number
off each.

A measured total lives beside the entry rather than in it. `FileEntry::size` for a
directory is the inode's own size, and writing a recursive total over it would make
one field that is sometimes one thing and sometimes another — a field nobody can
trust afterwards. Keyed by uri, so re-sorting or filtering cannot move a number onto
the wrong row, and dropped whenever the listing is replaced: a measurement describes
the tree as it was when it was taken, and a stale number is worse than an empty cell.
Sorting by size uses the measured total for folders that have one, which is the only
number anyone means when they sort a listing by size.

Two things the tests had to be dragged into being honest about. Cancellation cannot
be tested on a local disk: `folderSized` is queued to the test's thread, and by the
time the cancel is sent the worker has already finished the next folder, so both
answers arrive and the test proves nothing — it needed a drive that takes its time
listing, and then it asserts exactly one whole answer arrived. And "cancelled" must
never mean "reported half a folder as a total", because a wrong number in a listing
is worse than none, so the task checks for cancellation between finishing a folder
and announcing it.

Also covered: an empty folder answers zero rather than staying silent for ever, a
folder the walk cannot fully read reports what it managed, files are counted but the
directories in between are not, the ticked folders win over the whole listing when
there are any, and a refresh clears what was measured.

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
