# Changelog

New features and visible changes in Mole, newest first — **one sentence each**.
The reasoning belongs in [docs/adr/](docs/adr/) and the long account in
[DONE.md](DONE.md); repeating either one here only makes this file harder to scan.

## Unreleased

- Saving a drive now checks whether it can actually be reached and says so, instead
  of leaving a wrong endpoint or a refused password to surface later in the middle
  of browsing.
- Any configured drive can be checked on demand from the drives dialog.
- Network drives are now SFTP, FTP, S3 and WebDAV, asking only what each protocol
  needs instead of the eighty questions rclone's generated form could put on screen.
- One S3 drive type covers AWS, Backblaze B2, MinIO, Ceph, Wasabi and R2, because
  the endpoint and the addressing style are ordinary fields.
- rclone is gone, and with it 115 MB of Go and the `make librclone` step — along
  with Google Drive, Dropbox and OneDrive, which no open protocol reaches.
- A bucket whose name contains a dot no longer fails with a certificate error: it
  goes in the path, because a wildcard certificate cannot cover it in the host name.
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
