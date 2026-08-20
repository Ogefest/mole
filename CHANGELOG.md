# Changelog

New features and visible changes in Mole, newest first — **one line each**, in the
shape `YYYY-MM-DD #MOLE-nn what changed`. This file is the release notes: the
block between two release markers is one version's, pulled out by a regular
expression, so a line in another shape is a line nobody reads. The reasoning
belongs in [docs/adr/](docs/adr/) and the long account in [DONE.md](DONE.md).

The prose entries below the dated ones are from before 2026-08-10 and belong to
the first release; they stay as they are.

## Unreleased

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
