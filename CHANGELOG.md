# Changelog

New features and visible changes in Mole, newest first — **one line each**, in the
shape `YYYY-MM-DD #MOLE-nn what changed`. This file is the release notes: the
block between two release markers is one version's, pulled out by a regular
expression, so a line in another shape is a line nobody reads. The reasoning
belongs in [docs/adr/](docs/adr/) and the long account in [DONE.md](DONE.md).

The prose entries below the dated ones are from before 2026-08-10 and belong to
the first release; they stay as they are.

## Unreleased

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
