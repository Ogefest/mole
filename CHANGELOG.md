# Changelog

New features and visible changes in Mole, newest first. **This file is the release
notes**: a version's notes are the block below its own marker, pulled out by a
regular expression, so a line written in another shape is a line nobody will read.

Two shapes, and outside a fenced code block nothing else in this file may match
either of them:

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

An **entry** is the day it landed, the ticket, and the shortest phrase that says
what changed:

```
2026-08-11 #MOLE-112 A file with no newlines no longer stops the preview
```

A phrase and not a sentence. This is a list; the reasoning belongs in the commit
message — including where the first answer turned out to be wrong — and a
decision about shape in [docs/adr/](docs/adr/). See
[ADR-0071](docs/adr/0071-the-record-of-finished-work-is-the-commit.md) for why
there is no second telling of it anywhere, and
[ADR-0080](docs/adr/0080-the-changelog-is-a-structured-log-and-the-release-notes-come-out-of-it.md)
for this format.

**One line per change, not one per ticket.** A ticket that fixed two things
somebody would notice separately earns two lines, and they are not to be tidied
into one.

**`#MOLE-22`, never `#22`.** GitHub turns `#22` into a link to an issue that was
deleted on 2026-08-10, so a bare number in a published note is a dead link to
something that no longer exists; `#MOLE-22` links to nothing and reads correctly as
text. See [ADR-0022](docs/adr/0022-work-is-tracked-in-vikunja.md).

A **marker** says that everything below it — down to the next marker, or to the
end of the file — went out as that version:

```
## 0.2.0 — released 2026-08-11
```

Newest first, so cutting a release puts a marker in at the top and the entries it
contains fall underneath it.

**Every `##` line in this file is a release marker, and there are no other
second-level headings** — not `## Unreleased`, not one over the prose below, not
one somebody adds in a year. Everything above the topmost marker is unreleased by
definition, so a heading saying so would have to be moved on every cut, and any
other heading is one a looser expression would one day read as a release.

The bullets at the end of the file are prose from before this format. They belong
to the first release whatever shape they are in and they stay as they are:
recovering a date and a ticket for each one would be archaeology. Nothing is added
to them, and nothing dated goes below them.

A marker is written by `make release` and by nothing else. The tag that goes with
it is what publishes a release, so a second way to write one would be a second way
to publish something that nothing gated.

`tests/scripts/tst_Changelog.sh` holds every rule on this page, and it reads the
two expressions out of the block above rather than keeping a copy of its own — so
the file and the thing that checks it cannot come apart.

2026-09-04 #MOLE-382 An indexed search finds the same facts the details drawer shows, instead of missing files whose tags sit past the first page
2026-09-04 #MOLE-382 A metadata reader that fails on one file costs its own rows, instead of failing the whole scan
2026-09-04 #MOLE-382 Cancelling a scan stops the file being read, instead of finishing it first
2026-09-04 #MOLE-378 An alert on a drive that is unreachable says so, instead of reporting the file as missing
2026-09-04 #MOLE-378 A size, count or age alert over a folder it could not read says so, instead of measuring what it could reach
2026-09-04 #MOLE-378 A threshold that cannot be read is refused, instead of becoming 0 and firing on the first check
2026-09-04 #MOLE-378 An alert watching a "last modified" time no longer fires at a daylight-saving switch
2026-09-04 #MOLE-378 "Hours since anything changed" read from a report says it cannot be, instead of answering with the newest large file
2026-09-04 #MOLE-378 An alert saved by a newer Mole is dropped and counted, instead of loading as a watch on total size
2026-09-04 #MOLE-353 A chain says when its source could not be read, instead of finishing with nothing to do
2026-09-04 #MOLE-353 A table whose row count could not be taken shows a blank, instead of claiming to be empty
2026-09-04 #MOLE-406 Opening a file from an archive on a full disk says so, instead of opening half of it
2026-09-04 #MOLE-406 Extracting a large file for a viewer or an external program no longer freezes the window while it is written
2026-09-04 #MOLE-356 An index interrupted while upgrading its schema opens again next time, instead of failing for ever
2026-09-04 #MOLE-356 An index written by a newer Mole says so, instead of being opened and written to
2026-09-04 #MOLE-356 Two previews of one .sqlite file no longer blank each other out when either is closed
2026-09-04 #MOLE-356 An index or an imported table that cannot run in WAL mode says so, instead of quietly running slowly
2026-09-04 #MOLE-345 A WebDAV delete or move that the server only half did is reported as such, instead of as done
2026-09-04 #MOLE-345 Renaming into a folder that is not there says so, instead of "already exists"
2026-09-04 #MOLE-345 A WebDAV folder whose server answers under another address no longer lists itself as its own child
2026-09-04 #MOLE-348 A streamed download notices when the file changed underneath it, instead of handing back the first half of one file and the second half of another
2026-09-04 #MOLE-336 A sync says when a name is a file on one side and a folder on the other, instead of replacing one with the other or failing once per file inside it
2026-09-04 #MOLE-336 Writing a file where a folder stands is refused on every drive, instead of the folder being removed to make room
2026-09-04 #MOLE-334 A move to another disk goes through the guarded copy, instead of an unchecked block copy that could leave a truncated file under the final name
2026-09-04 #MOLE-334 A folder can be moved to another disk at all, instead of failing with "Cannot rename"
2026-09-03 #MOLE-335 A symbolic link is copied and moved as a link, instead of arriving as an empty folder or as a second copy of the file it points at
2026-09-03 #MOLE-335 A drive that cannot hold a link says which file it refused and why, instead of putting something else there
2026-09-03 #MOLE-335 Syncing a folder that contains a link back to itself finishes, instead of planning folders until the paths get too long
2026-09-03 #MOLE-351 A tab restored before its drive is connected keeps its place and comes back when the drive does
2026-09-03 #MOLE-351 Going back into an archive after leaving it opens it again, instead of showing an empty folder
2026-09-03 #MOLE-350 A session survives a launch where a plugin failed to load, and `mole --plugins` leaves it alone instead of rewriting it
2026-09-03 #MOLE-386 The console runner can create archives in the released builds, instead of reporting that libarchive was not found
2026-09-03 #MOLE-371 An indexed search filtered by date is answered by the index itself, and one that stopped at the row limit says so instead of reading as complete
2026-09-03 #MOLE-361 A dead network mount on the machine no longer stalls Mole at startup or when the sidebar refreshes
2026-09-03 #MOLE-360 Renaming, creating a folder, previewing a file, dragging a row and setting up a bulk rename no longer stop the window while a slow drive answers
2026-09-03 #MOLE-349 An FTP drive works on an account whose home is not the server root, instead of listing one place and deleting from another
2026-09-03 #MOLE-349 An FTP listing a server writes in its own format is read where possible and reported where not, instead of showing an empty folder
2026-09-03 #MOLE-349 An FTP password no longer appears in the session log when curl tracing is turned on, and TLS verification can be set from the drive form
2026-09-03 #MOLE-357 A damaged or hostile image, video or audio file cannot crash the details panel by lying about a length
2026-09-03 #MOLE-408 An operation on a filtered set acts on the rows the filter left, instead of on every member the set holds
2026-09-03 #MOLE-339 A delete removes exactly the files the confirmation listed, even if the folder changed or a tick moved while the question was on screen
2026-09-03 #MOLE-339 Removing a configured drive asks first, in red, instead of happening on the first click
2026-09-03 #MOLE-341 A duplicate scan no longer offers a link as a duplicate of the file it points at, and no longer pairs a file with itself when one search folder sits inside another
2026-09-03 #MOLE-341 A duplicate scan says how many places it could not read, instead of reporting "no duplicates" about a tree it could only partly open
2026-09-03 #MOLE-341 "Keep the copy nearest the top of the tree" keeps the one fewest folders deep, rather than the one with the shortest path text
2026-09-03 #MOLE-341 A duplicate delete that the drive refused keeps the results on screen with the reason, instead of emptying the tab and needing another scan
2026-09-03 #MOLE-352 An archive with non-ASCII names lists every member whatever locale the machine is in, instead of stopping at the first accented name
2026-09-03 #MOLE-352 A link inside an archive is shown as a link, and a hard link reads the file it points at, instead of both arriving as empty files
2026-09-03 #MOLE-352 An archive that is damaged, cut short, or only half recognised says so, instead of opening onto part of a tree or onto nothing
2026-09-03 #MOLE-352 A member whose name appears twice reads the version the listing describes, and a name with a backslash in a tar is one file rather than a folder
2026-09-03 #MOLE-347 Cancelling an upload to an object store now removes the parts it had already sent, instead of leaving them in the bucket being charged for
2026-09-03 #MOLE-347 An object or a folder over 5 GB on an object store can be renamed, instead of failing part way with keys copied under both names
2026-09-03 #MOLE-347 An object store says when an object last changed, and a key refused by a policy says so rather than reporting the file as missing
2026-09-03 #MOLE-347 A key with an empty segment in it no longer shows up as a folder inside itself, or as a row with no name
2026-09-03 #MOLE-346 An upload to a Windows share or an NFS export no longer overwrites a file that appeared while it was running
2026-09-03 #MOLE-343 A credential store survives a change of the cost its keys are derived at, instead of being orphaned by its own next write and reporting a wrong passphrase for ever
2026-09-03 #MOLE-343 A credential store that has been truncated or tampered with is refused rather than opened empty under any passphrase at all
2026-09-03 #MOLE-343 A credential that could not be written leaves the store exactly as the file has it, and a passphrase change that could not be written leaves the old passphrase working
2026-09-03 #MOLE-343 Typing the passphrase no longer stops the window while the key is worked out: the field says it is working and the button waits
2026-09-03 #MOLE-342 A setting, bookmark, set, schedule, alert rule, drive or session that could not be written says so, instead of showing the change on screen and losing it at the next start
2026-09-03 #MOLE-342 A settings file that has been damaged is kept beside itself rather than replaced by an empty one, so a stray comma in the drive list no longer costs every configured drive
2026-09-03 #MOLE-340 A nightly re-index keeps what is inside the archives in folders it did not have to re-walk, instead of dropping every member of them
2026-09-03 #MOLE-340 Searching a folder finds what is inside an archive in it, which the index knew about all along and could never be asked for
2026-09-03 #MOLE-340 A re-index that could not carry a folder forward reports it instead of finishing green with the folder missing
2026-09-03 #MOLE-340 A scheduled re-index waits rather than starting a second walk of a volume somebody is already scanning, and a rule made before archives could be asked for gets what the dialog would have asked
2026-09-03 #MOLE-338 An archive is refused rather than written with a member padded out with zeros when a file could not be read whole, so "remove the originals" can no longer delete the only good copy
2026-09-03 #MOLE-337 A mirror carries out the plan the confirmation showed, so a file that left the source between comparing and applying is no longer deleted at the far end without anybody being told
2026-09-03 #MOLE-337 A sync weighs what arrived at the destination, so a server that acknowledges bytes and stores fewer fails the run instead of leaving two trees that look like a backup
2026-09-03 #MOLE-337 A sync tab's patterns and switches reach the session file as they are typed, rather than only on a clean exit
2026-09-03 #MOLE-332 Moving a folder onto a folder of the same name merges the two and finishes the move, instead of copying the files and leaving the original where it was
2026-09-03 #MOLE-332 Moving a folder onto a folder of the same name within one drive no longer deletes what the destination folder held and the source does not
2026-09-03 #MOLE-333 A folder this account may not read is reported as unreadable instead of shown empty, so a mirror no longer empties the far end to match it
2026-09-03 #MOLE-333 A broken link, a named pipe, a socket and a device node appear in the listing, and a copy that cannot take one says so rather than passing over it in silence — a move that used to delete them without copying them now leaves them where they are
2026-09-03 #MOLE-331 A copy or move that overwrites a file and then fails part way through — a dropped connection, a full disk, a cancel — leaves the file it was replacing whole, instead of leaving neither it nor the replacement

## 0.1.2 — released 2026-09-02

2026-09-01 #MOLE-325 Mole says when a newer version has been released, once per version, with a button that opens its page — and Help → Check for new versions stops it looking at all
2026-09-01 #MOLE-316 A video on a machine with no decoder installed shows the file-information view instead of ending the application, and `mole --plugins` says what to install
2026-09-01 #MOLE-322 The tarball and the AppImage no longer carry a distribution's video codecs, which cuts the tarball from 114 MB to 79 MB and leaves video to the ffmpeg on your own machine

## 0.1.0 — released 2026-09-01

2026-09-01 #MOLE-318 Renaming a file on a Windows share to a different capitalisation works, and copying onto one warns before it overwrites a name that differs only in case
2026-08-24 #MOLE-311 Which drives the sidebar shows is a choice: the Drives dialog lists the disks the machine found beside the ones you set up, with a tick on each, and unticking one takes it off the strip without unmounting, unconfiguring or forgetting it
2026-08-24 #MOLE-310 Opening an archive to look inside it leaves the drive list alone: it is somewhere to walk around in until the last tab leaves it, and a bookmark or a restored session opens it again from the uri
2026-08-23 #MOLE-304 A download, a preview or a dragged copy that has to be staged is refused when there is nowhere to stage it, instead of being written somewhere nobody would look for it — and `MOLE_STAGING_DIR` says where staging happens, for a machine whose temporary directory is small or on the wrong disk
2026-08-23 #MOLE-309 A text preview numbers the lines of a file it holds whole, counting the file's own lines rather than the rows a long line was broken into for the layout, and shows no gutter at all for a file it can only show one window of
2026-08-23 #MOLE-308 Typing in a text preview finds a word in what is on screen, or `/` for an empty bar: the count says which of how many, Enter and Shift+Enter step through them, and where a file is shown a window at a time the count says that is what it counted
2026-08-23 #MOLE-125 A run that ends in a crash leaves a backtrace in the session log, on Linux and macOS alike
2026-08-23 #MOLE-306 A database write that fails says which failure it was — the file locked by another connection, a disk with no room left, or a database that moved on while this connection was reading it — in the index and in a file being imported alike
2026-08-23 #MOLE-301 A .jar, .war, .ear, .apk, .whl, .egg, .nupkg, .xpi, .vsix, .deb and .rpm open as drives like any other archive, because that is what they are — a document that happens to be a zip is deliberately still a document
2026-08-23 #MOLE-297 A listing sorts names naturally on a machine that names no language, where "file10" used to come before "file9" — a container, a cron job and a service started at boot are all in that state
2026-08-23 #MOLE-297 `mole --version` answers on a machine with no display, where it used to print nothing at all: the build somebody most needs to identify is the build that will not start
2026-08-23 #MOLE-297 A copy that has to be staged before it is sent is refused when there is no temporary directory to stage it in, instead of being written somewhere nobody would look for it
2026-08-23 #MOLE-291 An import that cannot commit a batch says so rather than finishing as though it had worked, so a table missing two thousand rows is never presented as a whole one
2026-08-23 #MOLE-287 A Parquet file written as one row group no longer holds the window while it is read whole: a window decodes a batch at a time and stops where the answer does, and the grid reads its rows, its filtered counts and its column widths on a task
2026-08-23 #MOLE-286 A document with a heavy page no longer holds the window while it draws: a page is rasterised on a task, the strip says which one is being drawn, and stepping off a document abandons what it had outstanding
2026-08-23 #MOLE-288 Asking to see a very large image at full size says it cannot rather than emptying the pane: the button is offered only where this build of Qt can decode the whole picture, and says how large it is where it cannot
2026-08-23 #MOLE-290 Moving to another file while a large one is still being imported no longer risks taking the application with it: the import holds the scratch database it writes to, so it runs out on its own and reports nothing, and every connection it opened is closed when the store goes
2026-08-23 #MOLE-289 A long file of records can be read while it is still being imported, instead of freezing the window for seconds at a time and then reading as an empty table — the scratch database is in WAL with a busy timeout on every connection, and a read that fails now says so rather than answering as a file with nothing in it
2026-08-22 #MOLE-285 A file of JSON records opens as a table when its records are flat, with nested values shown as JSON in the cell — paged, filtered and counted over the whole file like any other grid, and shown as source when the records are not objects
2026-08-22 #MOLE-284 A preview that reads a file and cannot show it says so and falls back to the next viewer down instead of leaving an empty pane — a video with no decoder and an image this build cannot decode now open the file's own facts, with a line saying which viewer gave up
2026-08-22 #MOLE-283 A Markdown file with a very large table opens as source instead of freezing the window, says why, and can still be rendered from the strip — where Markdown now offers Show: Rendered or Source like an HTML file does
2026-08-22 #MOLE-282 A mounted drive is asked its own name rules, its own case folding and its own leftovers again, so a copy onto a stricter volume is checked before it starts, the guard against moving a folder into itself gets case right, and the sweep can find what an interrupted upload left behind
2026-08-22 #MOLE-201 A container that keeps earlier objects lists them for a file, marks the rows that have them and reads one back — asked once per container, paginated properly, and silent on a container that does not
2026-08-22 #MOLE-200 On a filesystem that keeps snapshots, a file's earlier states are listed, marked in the folder and opened like any other file — read-only, discovered from the paths that are already there, and costing nothing on a machine that has none
2026-08-22 #MOLE-203 The guide says that drives differ and that this is the point: what a drive offers depends on what it was pointed at, how an earlier version is read and copied out, that nothing here writes, and that a drive with nothing to offer costs nothing
2026-08-22 #MOLE-202 An object store can hand out a link to one object that works for fifteen minutes without an account, shown with its expiry and copied from the same place any drive's answer is
2026-08-22 #MOLE-199 An earlier version of a file can be looked at in the preview without leaving it: the picker beside the file name moves between them, every viewer works on one because it is an ordinary file, and the screen always says which version is on
2026-08-22 #MOLE-198 A listing marks the rows a drive has something for — one query for the whole folder rather than one per row, so a folder of five thousand files draws as fast as it did and the marks arrive after
2026-08-22 #MOLE-197 What a drive can do to the file under the cursor appears under Operations, whatever drive it is: an answer that is text is shown with a way to copy it and when it stops working, and an answer that is a list of earlier versions is a list you open one from
2026-08-22 #MOLE-196 The test suite has a drive that contributes actions and keeps readable earlier versions of a file, so everything built on a drive's own actions is tested without a server or a particular filesystem
2026-08-22 #MOLE-195 A uri can name which state of a file is meant, so an earlier version is an ordinary file that any viewer opens and any bookmark can point at — and a drive that does not keep earlier states refuses one instead of quietly showing the current file
2026-08-22 #MOLE-194 What a drive can offer is discovered from the drive when a folder on it is first opened, rather than compiled into its class — a drive nobody opens is never asked, one that cannot say goes on working, and "not asked yet" no longer reads as "no"
2026-08-21 #MOLE-193 A drive can offer an action no other drive has, and answers with a piece of text or a list of alternate uris for the same file — nothing between the backend and the menu learns what the action was
2026-08-21 #MOLE-281 The guide says how to choose a theme, which four there are, and that its pictures are all taken in Midnight
2026-08-21 #MOLE-280 Two light themes, Paper and Workbench, and the controls, previews and code colours that follow a theme's polarity; the active view mode and every view's primary button are now legible rather than a drop shadow
2026-08-21 #MOLE-279 Mole ships two themes, Midnight and Slate, and the View menu picks between them; the choice is remembered
2026-08-21 #MOLE-278 Colour comes from one palette rather than 372 values in 39 files: the automation tab and the terminal panel now use the same greys as the rest of the window, and the sidebar's capacity bar the same blue
2026-08-21 #MOLE-274 Forgetting an index no longer freezes the window while a scan is writing, and a removal that fails says so instead of looking like one that worked
2026-08-21 #MOLE-277 A tab opened from another no longer carries a "Back to" bar above it; closing it still returns you to the tab it came from
2026-08-21 #MOLE-272 The search form's criteria scroll, so the size range and the index toggle are reachable instead of sitting under the task strip
2026-08-21 #MOLE-273 make tsan is green again, and a test suite that leaves a task running is now a failure somebody sees
2026-08-21 #MOLE-275 Moving a directory into its own subdirectory on one drive is refused instead of silently relabelling the tree underneath itself
2026-08-21 #MOLE-250 Opening a file from a remote drive stages it inside the scratch folder, and two servers holding the same path no longer overwrite each other
2026-08-21 #MOLE-243 A copy or a rename says which name the drive will not accept, and which character is the problem, instead of failing part way through with a path
2026-08-21 #MOLE-241 The drive list finds a disk wherever it is mounted, including a separate /home, and stops listing a ramdisk as a drive
2026-08-21 #MOLE-181 The sidebar lists drives on Windows and macOS as well as Linux, with the name the disk carries rather than "Root"
2026-08-21 #MOLE-242 Renaming a file to a different capitalisation of its own name works, instead of being refused as one that would overwrite something
2026-08-21 #MOLE-240 On a volume that ignores case, two spellings of one folder are one folder, so a move cannot be slipped past the guard that stops a directory being put inside itself
2026-08-21 #MOLE-180 Local paths with a drive letter and Windows shares are handled correctly, so a Windows build can reach the local disk
2026-08-21 #MOLE-271 The search line can be typed into: it was never connected to anything
2026-08-21 #MOLE-271 A search tab is one box with the keyboard already in it, and everything else is behind More
2026-08-21 #MOLE-264 The window no longer waits on the index: what is indexed is read in the background and answered from memory
2026-08-21 #MOLE-269 A search, or the list of indexes, no longer waits for a running scan to stop writing
2026-08-21 #MOLE-270 Search form: every criterion label sits beside the field it names
2026-08-21 #MOLE-266 The list of what an operation is aimed at draws its icons from a scalable font, so they are sharp and the guide's pictures hold still
2026-08-21 #MOLE-259 Saved reports, Indexes, Alerts and Schedule show the tab you already have instead of opening another
2026-08-21 #MOLE-268 A scheduled job killed part way is due again after its interval, not at the next start and every start after that
2026-08-20 #MOLE-261 A picture that moves says where it moved and keeps both copies, so an unexplained one explains itself next time
2026-08-20 #MOLE-264 Scheduled jobs wait a few seconds after startup, so the window comes up before the work does
2026-08-20 #MOLE-265 Setting an index's Repeat no longer crashes the application
2026-08-20 #MOLE-265 A crash's backtrace survives the restarts that follow it, instead of being rotated away by the second one
2026-08-20 #MOLE-258 The task strip lists and counts the jobs you asked for, not the hundreds of thumbnails and listings browsing produces
2026-08-20 #MOLE-263 A session log opens with what the run started with: the build, the plugins, the drives and their state, the indexes and the restored session
2026-08-20 #MOLE-262 A session log says what ran without being asked: every job's start, end and duration, with browsing and housekeeping still quiet
2026-08-20 #MOLE-260 The compress and delete dialogs no longer flash a scrollbar over a list that fits
2026-08-20 #MOLE-257 The guide says a set is somewhere you can go: walking the members, Ctrl+D, and a picture of both kinds of bookmark
2026-08-20 #MOLE-256 A test that clicks something clicks it where the layout put it, and a failing run says where its log is
2026-08-20 #MOLE-254 Building a set from a duplicate scan or a search shows the Sets tab that is already open, pointed at the new set
2026-08-20 #MOLE-255 Regenerating the guide's pictures rewrites only what changed, and the terminal picture no longer carries a real machine name
2026-08-20 #MOLE-255 A sync plan and a duplicate group are listed in a stable order, so a second look matches the first
2026-08-20 #MOLE-235 A killed test run's payload is swept from the test server before the next run, so a skip for want of room is never our own litter
2026-08-20 #MOLE-233 Shell scripts are tested: a stub ssh holds the NFS export guard, and every script is parsed and checked statically
2026-08-20 #MOLE-209 A bookmark row says whether it is a folder or a set, a dead one reads as dead, and Ctrl+D in the Sets tab bookmarks the set
2026-08-20 #MOLE-208 A bookmarked set opens the Sets tab with that set current, from the sidebar, the menu or the palette
2026-08-20 #MOLE-207 A bookmark can point at a set as well as a folder, by id, so a rename follows and a deleted set reads as dead
2026-08-20 #MOLE-206 Add to set shows the Sets tab that is already open instead of opening another
2026-08-20 #MOLE-205 A set's members have a cursor: arrows walk them, Enter opens the member itself and F3 previews it
2026-08-20 #MOLE-204 F3 in a search result previews the row under the cursor instead of doing nothing
2026-08-20 #MOLE-238 Starting Mole no longer builds the GStreamer stack, which cost most of a second
2026-08-20 #MOLE-239 A video thumbnail seeks to its frame, so a file over a minute gets a tile at all
2026-08-20 #MOLE-237 Fix the sanitizer tier going red on GStreamer's device monitor and on a pooled NFS mount
2026-08-20 #MOLE-144 A folder of PDFs shows first pages and a folder of videos shows frames — not frame zero, which is black in a great many of them
2026-08-20 #MOLE-143 A gallery over a network drive now reads the thumbnail a camera already put in each photograph — kilobytes rather than megabytes — and leaves a file past a ceiling alone rather than downloading it
2026-08-20 #MOLE-142 A flick through a folder of photographs now makes thumbnails for what is on screen, a few at a time, newest request first — and leaving a folder takes its queue with it
2026-08-20 #MOLE-141 Thumbnails are kept, in memory for the scroll and on disk for the next visit, so a folder of photographs is decoded once rather than on every look
2026-08-20 #MOLE-140 The gallery now shows what pictures look like: thumbnails are a new extension point, made off the UI thread, bounded in size, and in the right orientation for a photograph taken upright
2026-08-20 #MOLE-139 A fourth way of looking at a folder: Gallery, with tiles big enough to see a picture in — the same pane, so selection, sorting, filter-by-typing and F3 all work in it
2026-08-20 #MOLE-138 In a grid of tiles the left and right arrows now move a tile and up and down move a visual row, and Page Up/Down move by the number of entries actually on screen instead of by fifteen
2026-08-20 #MOLE-231 An index can now be rescanned, put on a clock or forgotten from the Indexes tab, and a scan that is running shows on its row with a way to stop it
2026-08-20 #MOLE-230 A new Indexes tab lists every indexed tree — how old it is, how big it is, what kind of scan built it, and whether anything is keeping it fresh
2026-08-20 #MOLE-228 Index this folder now opens the indexing dialog on that folder, instead of quietly starting a full walk that recorded nothing about the files
2026-08-19 #MOLE-227 A folder's re-index can be set to any interval from hourly to monthly, changed afterwards, and turned off again — it was a checkbox that only ever created a rule, fixed at 24 hours
2026-08-19 #MOLE-226 A scheduled re-index now repeats the scan that created it — a folder indexed with metadata or archive contents no longer loses them, a subtree at a time, every night
2026-08-19 #MOLE-126 A sync now reports how much it has copied — the figure and the progress bar used to stop advancing part way through and settle on a fraction of the real total, while the copy itself was correct all along
2026-08-19 #MOLE-224 The drives dialog's Kind picker now names the drive you selected, instead of the one you looked at before
2026-08-19 #MOLE-225 A video preview can be muted, and remembers it — for every video and across restarts
2026-08-19 #MOLE-223 A video preview starts playing when it opens, instead of waiting on the Play button
2026-08-19 #MOLE-210 A duplicate scan no longer stops the window responding as it fills — a confirmed group is added to the list instead of rebuilding every group already in it, so the results can be read and scrolled while the scan is still running
2026-08-19 #MOLE-222 A drive being worked on breathes gently rather than flickering — the disk activity light was accurate and read as an alarm next to everything else in the sidebar
2026-08-19 #MOLE-221 A drive being worked on is now green instead of pulsing in the same blue that means "the drive you are looking at"
2026-08-19 #MOLE-162 A drive shows when Mole is working on it — a copy makes both ends pulse in the sidebar and stops the moment it finishes, so which drives a transfer is touching is visible rather than something to read out of a task title
2026-08-19 #MOLE-161 A drive's dot in the sidebar now says what the drive is doing rather than whether it is plugged in — filled grey for here and quiet, blue for one you are looking at, a hollow ring for one that is not connected, and a pulsing ring while it connects
2026-08-19 #MOLE-37 F3 on a video now plays it — the file opens paused at its first frame with a play button, a position and somewhere to drag it to, and a build without Qt Multimedia shows the file's details as before
2026-08-19 #MOLE-128 A dialog, a menu or the command palette no longer washes the window behind it out to a pale grey — Qt's dark-theme dim is near-white at sixty percent, and what sits behind a popup is now dimmed instead of faded almost away
2026-08-19 #MOLE-219 F3 on a file compressed on its own now shows what is inside it — a gzipped log opens in the text viewer, a gzipped CSV as a table and a gzipped photograph as the picture, instead of a list of properties
2026-08-19 #MOLE-218 A file inside an archive is decompressed as it is read instead of all at once, so previewing a member of any size opens at once and costs its window rather than the whole file
2026-08-19 #MOLE-216 A file compressed on its own — a `.gz`, `.xz`, `.bz2` or `.zst` with no tar inside — now opens as a drive instead of being offered and then refused, with its one row named after the file that was compressed rather than "data"
2026-08-19 #MOLE-215 A sync that compares by contents is noticeably quicker over fast storage — the two files are compared byte for byte instead of both being hashed with SHA-256, and two that differ now stop at the first block that differs instead of being read to the end
2026-08-19 #MOLE-214 A duplicate scan by content is much faster, and what it reports is now exact — the reads are spread across threads instead of running on one, nothing in the scan is capped by a digest any more, and a group reported as identical has been compared byte for byte rather than hashed
2026-08-19 #MOLE-213 Mole can now open an NFS export as a drive, in the application and without mounting it or needing root
2026-08-19 #MOLE-36 Mole can now open a Windows or NAS share (SMB) as a drive, without mounting it and without root
2026-08-19 #MOLE-212 A transfer that stops moving is given up on when Mole's own guard says so, on every protocol, instead of when the operating system happens to notice — and cancelling one takes effect at once
2026-08-19 #MOLE-108 A transfer over a link that drops and comes back is no longer lost — a read keeps retrying from where it got to while bytes are still arriving, and gives up only once nothing has arrived for two minutes
2026-08-19 #MOLE-96 An S3 drive can now be asked what uploads it never finished — the parts a killed process leaves behind are charged for until somebody removes them, and until now nothing in Mole could even see them
2026-08-19 #MOLE-99 A large file can now be read from an SFTP server that re-keys part way through a span — the read resumes from the byte it reached instead of failing, so a file that could not be read at all now arrives whole
2026-08-19 #MOLE-98 Sync no longer takes a dropped connection for the end of a file — a read that dies half way is a failure instead of a file counted as copied, and a destination that fills up says so instead of saying "short write"
2026-08-19 #MOLE-127 An FTP drive can now read a file larger than the local scratch space — reads stream a span at a time instead of downloading the whole file first, which was the last place a backend staged anything
2026-08-18 #MOLE-187 A table, a database or a Parquet file is now shown five thousand rows at a time, with page controls under the grid, instead of one scrollbar over the whole of it
2026-08-18 #MOLE-186 A database with large tables now opens at once and fills its row counts in behind, instead of holding the window until every table has been counted — and a filter typed into a table, a CSV or a Parquet file is now scanned once when the typing stops rather than once per character
2026-08-18 #MOLE-188 Analysing a large folder no longer freezes the window while it runs — every task's status line and published counts are now handed to the interface ten times a second at most, however fast the work produces them
2026-08-18 #MOLE-191 Identical contents compares the first megabyte instead of the first 16 kB, so video, RAW photographs, PDFs and disk images that merely share a header stop reaching the whole-file hash
2026-08-18 #MOLE-190 Fix every dropdown in Mole cutting its longest name in half — the list is now as wide as the names in it, in the same text size as the control
2026-08-18 #MOLE-72 Choosing what to keep is a panel rather than four flat buttons — a rule says what it did across every group, each copy reads keeping or remove, and any group can be overridden with one click
2026-08-18 #MOLE-71 Ticked duplicates can become a file set instead of a deletion, and an operation invoked from the menu over a duplicates tab acts on what is ticked
2026-08-18 #MOLE-70 Duplicate groups appear as the scan confirms them instead of all at the end, largest saving first at every instant, with the stage and file count on screen — and stopping keeps what was found rather than claiming nothing matched
2026-08-18 #MOLE-69 A duplicates tab fills its own space in every state and says which one it is in — what will be searched and what it costs before a scan, progress during one, and one row per folder instead of an elided join
2026-08-18 #MOLE-184 The count of changed files in the git band can now be opened, which is the only way to see a file git has been told to delete
2026-08-18 #MOLE-107 The guide explains the git band and the letters on the rows, and the read-only boundary is written down as a decision rather than a gap
2026-08-18 #MOLE-106 The band says how far the branch is from what it tracks and what the last commit was — the subject elided rather than wrapping, and nothing at all when there is no upstream or no commit
2026-08-18 #MOLE-105 Git markers keep up with the tree: an operation that writes, or a commit made in the terminal or another window, refreshes them without losing the cursor or the ticked rows
2026-08-18 #MOLE-185 Fix every index search iterating a list that had already been destroyed
2026-08-18 #MOLE-104 Rows in a checkout carry git's own letter — M, A, D, ??, R, U — and a folder says when something inside it has changed, at every level up to the one on screen
2026-08-18 #MOLE-103 The band above a listing says how much of the checkout has changed, or that it is clean — one walk of the work tree per checkout rather than one per folder, on a worker, and abandoned when you navigate out of it
2026-08-18 #MOLE-102 A folder inside a git checkout says which branch it is on, in a band above the listing — the state instead when git is part-way through a rebase or a merge, the commit when HEAD is detached, and no band at all for a folder that is not a checkout
2026-08-11 #MOLE-88 A file on an archive or a network drive can be dragged out too: the first drag fetches it and says so, the next one carries it, and dragging the same file twice fetches it once
2026-08-11 #MOLE-87 Files dropped onto a listing are copied into the folder it is showing, with the pane saying how many and where while the pointer is still moving, and the copy confirmation asking before any name is overwritten
2026-08-11 #MOLE-85 Files can be dragged out of a listing onto anything that takes files — the ticked rows when you start on one of them, that row alone when you do not, always as a copy
2026-08-11 #MOLE-160 The details are a drawer beside the preview that you can read down, select and copy out of, and put away — one switch for every file rather than one per type
2026-08-11 #MOLE-159 A file Mole can name — a video, an mp3, a .docx — shows what it is rather than a hex dump, and the bytes are a choice on the strip
2026-08-11 #MOLE-156 A query line above the form, where the two are one query seen twice — typing moves the fields and changing a field rewrites the line, and a query nobody can read says so instead of matching everything
2026-08-11 #MOLE-155 A re-scan keeps what has not changed instead of walking the tree again, and a folder can be put on a nightly clock that survives a restart and catches up on a run it missed
2026-08-11 #MOLE-154 A scan lists what is inside the archives it meets, so a file in a zip is found by name and opening the row lands on it inside the archive
2026-08-11 #MOLE-153 The search says in one line what the folder it is aimed at can be asked, offers a field for every fact that has been recorded there, and stops rather than quietly widening when asked something the scope has no record of
2026-08-11 #MOLE-152 A scan can record what each file says about itself — a camera, an author, a duration — so those can be searched for later without the file being opened, while the contents themselves stay out of the index
2026-08-11 #MOLE-151 A search can look inside the files — literal or expression, text files only unless asked otherwise, bounded and cancellable, with each hit showing the line it was found on
2026-08-11 #MOLE-150 The search asks for time, for what a file is rather than what it is called, for a name as a shape or an expression, for a path, for folders to skip and for depth — nine families of criteria where there were three
2026-08-11 #MOLE-149 A folder with an indexed subtree in it is answered by both at once — the indexed part appears instantly and marked as remembered, and the walk replaces each row as it reaches it, removing what has been deleted since the scan
2026-08-11 #MOLE-148 One search, with where to look as a field on it — this folder, a path, or everywhere indexed — and the index as a way of answering it rather than a separate tab
2026-08-11 #MOLE-158 Examining a search's results leaves one browser tab rather than one per result, the results stay where they were, and the tab it opens says which search it came from
2026-08-11 #MOLE-146 A rescan no longer empties the index while it runs, so a search during one answers from the previous scan in full, and a rescan that is cancelled, fails or is killed leaves the index exactly as it was
2026-08-11 #MOLE-136 An audio file's details say its title, artist and album, with the duration exact where the file states it and labelled as an estimate where it does not
2026-08-11 #MOLE-135 A video's details say how long it runs, how big the picture is and what it is coded in, read from the container's header
2026-08-11 #MOLE-134 A PDF's details name its title, author and page count, and a .docx or .odt names its author without the file being read whole
2026-08-11 #MOLE-133 A photograph's details say its dimensions, its camera, the exposure and where it was taken, read from the header rather than by decoding it
2026-08-11 #MOLE-132 Every preview has a Details panel saying what the file says about itself, and a plugin can fill it for a format without shipping a viewer
2026-08-11 #MOLE-131 A file no viewer understands is shown as bytes — offsets, hex and text, paged, with a selection that copies as either
2026-08-11 #MOLE-130 A text file whose name Mole does not recognise now opens as text, and Dockerfiles, makefiles and CMakeLists.txt are coloured
2026-08-11 #MOLE-129 The preview layer identifies a file by what is in it rather than by what it is called, so a Dockerfile opens as text and a zip renamed notes.txt does not
2026-08-10 #MOLE-34 A file bigger than the local disk can be written to an FTP drive, which was the last backend that staged the whole upload first
2026-08-10 #MOLE-30 A task that measures bytes says how long is left
2026-08-10 #MOLE-73 The File menu offers a new tab only for what can be opened from nothing, instead of thirteen entries of which five open onto an empty view
2026-08-10 #MOLE-111 A drive whose password is in the credential store is connected the first time you open it, and asks for the passphrase then, instead of asking at startup for a drive you may never touch
2026-08-10 #MOLE-111 Opening a configured drive that is not connected connects it, instead of showing it as a folder with nothing in it
2026-08-10 #MOLE-100 Every dialog says which button acts, and can be answered from the keyboard
2026-08-10 #MOLE-100 Copying or moving a single file can rename it on the way, which the field for it never allowed
2026-08-10 #MOLE-112 A file with no line breaks in it — a minified export, a one-line dump — previews instead of stopping the window
2026-08-10 #MOLE-93 Plain text and source previews are drawn a step larger, level with the rest of the interface
2026-08-10 #MOLE-110 `make asan` is green again
2026-08-10 #MOLE-13 A bulk rename that swaps or shifts names now works instead of refusing every row
2026-08-10 #MOLE-2 A backslash in a file name is part of the name, not a folder separator
2026-08-10 #MOLE-2 A file with a name at the length limit can be copied
2026-08-10 #MOLE-16 A progress bar can no longer read past 100% or slide backwards
2026-08-10 #MOLE-16 A task that throws is reported as failed instead of ending the process
2026-08-10 #MOLE-10 An archive entry named `../` can no longer address anything outside the archive
2026-08-10 #MOLE-12 A file changed while a duplicate scan reads it is no longer offered as a copy of anything
2026-08-10 #MOLE-11 A mirror no longer deletes the destination when it cannot read the source
2026-08-10 #MOLE-3 A job's duration and speed no longer go wrong when the system clock is stepped
2026-08-10 #MOLE-9 Deleting a shortcut to a folder no longer empties the folder it points at
2026-08-10 #MOLE-9 The drive root cannot be deleted
2026-08-10 #MOLE-8 A move no longer deletes a file it skipped
2026-08-10 #MOLE-8 A folder can no longer be moved or copied into itself

- `mole-tasks` runs any of the work from a console — copy, move, delete, sync,
  compress, rename, scan, duplicates, verify — with no window and no display.
- A copy that is cancelled, or that fails part way through, no longer leaves what
  had arrived behind under the name it was aiming at.
- A copy whose source stops handing over bytes early is reported as a failure
  rather than as a finished file, unless the file really did get smaller.
- A copy that could not be written says why — the disk being full and the
  connection going away no longer read alike.
- Writing a small file to a WebDAV drive works; like the large case before it, any
  server that asks for a password refused it.
- Opening a WebDAV folder as if it were a file is refused, instead of handing back
  the server's HTML index of the folder as the file's contents.
- A file being uploaded is never re-sent to somewhere else because a server answered
  with a redirect.

- A copy interrupted by the machine losing power no longer destroys the file it was
  replacing: what is left is marked `.mole-partial` and the previous version is
  untouched.
- Preferences survive a crash during a save, instead of being emptied.
- Writing anything larger than a few hundred bytes to a WebDAV drive works;
  it previously failed with "necessary data rewind wasn't possible" on any server
  that asks for a password.
- An upload to SFTP, FTP or WebDAV that is killed before it finishes now leaves a
  file marked `.mole-partial` rather than a half-written file under the name it
  was meant to have.
- `F3` and `Ctrl+←/→/↑` work wherever the keyboard is, instead of going dead until
  the file listing is clicked, and clicking a drive hands the keyboard back.
- Drives can be connected, ejected and checked from the command palette on `Ctrl+R`,
  by name, without reaching for the pointer.
- A drive whose password is in a locked store now says so in the window, with one place
  to type the passphrase, instead of waiting in silence until somebody opens a dialog.
- A drive is shown as connected only once something has actually reached it, and turns
  red with the reason when it stops answering.
- A drive connects, ejects and can be checked from its own row in the sidebar, with a
  coloured dot for what it is doing, instead of through the drives dialog.
- The sidebar lists every drive that has been set up, not only the ones connected
  right now, and says which each one is: connected, not connected, or locked.
- A window left full-screen comes back full-screen, and keeps the size it had before
  instead of reopening as a plain window the size of the screen.
- Search results can be scrolled through while the search is still running: arriving
  results no longer throw away the position or the row under the cursor.
- Previewing a Markdown file no longer risks taking the application down with it, and
  a long one is now styled once rather than once per piece of text Qt parses.
- Cancelling a job no longer leaves a warning in the log calling it a failure, so what
  is left there is the trouble worth reading about.
- Every copied file is weighed at the destination afterwards, and a copy that landed
  short now fails instead of being counted as done — a move keeps the original.
- Objects larger than 5 GB can be written to S3, which now uploads in parts, and a
  large WebDAV write is sent as it is produced rather than collected first.
- A file of any size can be copied to or from an SFTP drive without needing room for
  a second copy of it locally: transfers now stream instead of being downloaded or
  collected in full first, so a 100 GB backup costs a few megabytes of memory.
- Large files can be copied from an SFTP drive: a transfer used to stop dead a little
  short of a gigabyte and either time out or leave part of a file behind.
- A large copy shows progress from the first second, and a preview of a huge remote
  file reads the first page instead of fetching the whole thing.
- A download that ends short of the length the server announced is now reported as a
  failure instead of being handed over as a complete file.
- A copy whose source stops responding half way is reported as a failure rather than
  finishing quietly with a truncated file.
- `MOLE_LOG=task,drive,net,curl` records what every job and every drive did into the
  session log, for working out why something went wrong.
- The path of the open folder, of the selected file, or of the drive can be copied to
  the clipboard — `Ctrl+Shift+C` and `Ctrl+Shift+F`, or from the Operations menu.
- A notification no longer stops every keyboard shortcut from working for as long as
  it is on screen.
- Drive and bookmark rows in the sidebar are taller, evenly sized, and their names
  no longer shift sideways when the mouse passes over them.
- Saving a drive now checks whether it can actually be reached and says so, instead
  of leaving a wrong endpoint or a refused password to surface later in the middle
  of browsing.
- Any configured drive can be checked on demand from the drives dialog.
- A bucket whose name contains a dot no longer fails with a certificate error: it
  goes in the path, because a wildcard certificate cannot cover it in the host name.
- Network drives are now SFTP, FTP, S3 and WebDAV, asking only what each protocol
  needs instead of the eighty questions rclone's generated form could put on screen.
- One S3 drive type covers AWS, Backblaze B2, MinIO, Ceph, Wasabi and R2, because
  the endpoint and the addressing style are ordinary fields.
- rclone is gone, and with it 115 MB of Go and the `make librclone` step — along
  with Google Drive, Dropbox and OneDrive, which no open protocol reaches.
- A copy onto a remote drive that fails while being sent is now reported as a
  failure instead of quietly looking like it worked.
- The command palette offers the drives again, so a drive can be reached by typing
  instead of clicking the list on the left.
- Dialog buttons say what they do, the one that acts is filled — red when it cannot be
  undone — and a destructive dialog opens with the keyboard on the way out.
- Compressing can delete the originals once the archive is written, and keeps them if
  anything could not be read.
- The dialogs that delete, sync or pack now list the files by name instead of counting
  them.
- Compressing offers 7z and a bare xz as well, and changing the kind no longer discards
  the name that was typed.
- Compressing lists exactly what it is about to pack, acts on the row under the cursor
  when nothing is ticked, and can protect a zip with a password.
- A user guide in docs/guide/, illustrated with screenshots the test suite takes
  immediately after asserting what they show.
- The selection can be compressed into a new zip, tar.gz or tar.xz beside it, packed
  in the background.
- Search results can be walked with the arrows, opened in their folder with Enter, and
  turned into a set from a button beside them.
- An HTML file can be shown as source or as a page, and the choice is remembered for
  the next file of that type.
- A long search no longer makes the window stop responding: streaming results into
  the list is near-linear instead of re-sorting everything on every batch.
- Search results appear as they are found, can be narrowed where they are, and can be
  turned into a file set to carry on working with.
- The licence check says which launcher it expected when a bundle in dist/ is not
  this project's.
- A search-commands bar sits in the middle of the title bar, showing the Ctrl+R that
  opens it.
- The command palette opens on Ctrl+R, instantly, and always starts with an empty
  box.
- Ctrl+F puts the keyboard in the search box, says when a search is running, filters
  by size, and answers from the index when it covers the folder.
- Ctrl+Shift+P opens one box that finds and runs any command, bookmark or drive by
  typing part of its name.
- PDFs open as pages instead of falling through to the information viewer, rendered
  one page at a time as they are reached.
- Bulk rename shows the result as you type, and the preview is no longer squeezed
  out by the form.
- The + and × buttons, and every other icon-only control that was smaller, are big
  enough to hit, with the glyph sized to match.
- Text in the listing, the previews and the sidebar is larger and comes from one
  shared scale instead of a number picked per view.
- Ctrl+Shift+S measures the folders in front of you in the background and writes
  their sizes into the listing as each one lands.
- F3 on a folder opens it, instead of doing nothing because there is nothing to
  preview.
- The F4 menu separates what acts on the files in front of you (Operations) from
  what opens a tool to work in (Workflows), instead of one Tools heap.
- The F4 menu can be walked entirely from the keyboard: arrows move, Right or
  Enter opens a submenu, and Left or Escape comes back out.
- The terminal panel takes the keyboard when it opens, and Ctrl+D ends the shell
  and closes the panel instead of bookmarking the folder.
- A large CSV now fills the grid as it is read, and says it is reading before the
  first rows arrive, instead of showing an empty view that looked like a hang.
- Markdown previews are set as a page rather than a wall of text.
