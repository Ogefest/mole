# ADR-0033: A file is identified by its contents, in a second lookup pass

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

`IPreviewProvider::canPreview()` has always been a cheap test on a name: *"based
on name, suffix and size. Must not do any I/O"*. Every provider obeys it, so the
whole preview layer decides what a file is from what it is called and never from
what is in it.

The names that carry no suffix are the ones that suffer. `shared-mime-info` 2.4
has no glob for `Dockerfile`, `Jenkinsfile`, `LICENSE`, `.bashrc` or
`.editorconfig`, so each of them is `application/octet-stream` to a name-only
lookup: the text viewer declines, the fact list claims them, and a file that is
plainly text shows nine numbers out of `stat()`. The reverse fails too — a zip
renamed `notes.txt` opens in the text viewer and fills the window with
replacement characters.

`FileEntry::mimeType` has carried the comment *"Set only when the backend
already knows it for free. Otherwise the preview layer sniffs the type lazily"*
since it was written. Nothing sniffed anything, and no backend fills the field
either, so it has always been empty.

## Decision

**A file that the name cannot place is read — one page of it — and identified
from those bytes.**

Three parts:

1. `FileType::identify()` in `core`, given a name and up to
   `FileType::kSampleBytes` (4096) of the head, answers one MIME type. It reads
   nothing itself.
2. `PreviewTabController::showEntry()` runs a **second lookup pass**. Pass one is
   the name-only lookup, exactly as before. When it lands on a provider whose
   `priority()` is below zero — the fallback tier, today the text viewer at -100
   and the fact list at -1000 — the head is read through `ReadRangeTask` on a
   worker thread, the answer is put in `FileEntry::mimeType`, and the registry is
   asked again. A provider above that tier has claimed the file on its name and
   is not second-guessed.
3. Providers read `entry.mimeType` when it is set and let it decide.
   `canPreview()` keeps its no-I/O contract: it is asked twice rather than handed
   a stream, and `PreviewRegistry` still reads nothing.

Deciding between the name's answer and the bytes' answer is Mole's own rule, not
Qt's. See *Reason*.

## Reason

**Why a second pass rather than widening `canPreview()`.** Handing providers a
stream would put I/O in a call the registry makes over every provider for every
file, on the UI thread, and a plugin that read carelessly would stall the window.
Asking the same cheap question twice, with more known the second time, costs one
read for the files that need it and nothing at all for the files that do not.

**Why only below priority zero.** A `.png` is claimed by the image viewer on its
name and does not need proving; a `.csv` is claimed by the table viewer. The
fallback tier is exactly the set of files where nothing was recognised, which is
where a read can still change the answer. It also bounds the cost: no extra read
for any file that already had a viewer of its own.

**Why 4 kB.** Every magic rule in `shared-mime-info` matches within the first few
hundred bytes, and the text test is a sample rather than a survey. A 100 GB mbox
with no extension has to open as fast as a 100 byte one, which is the same reason
the text viewer reads 512 kB windows rather than files.

**Why not `QMimeDatabase::mimeTypeForFileNameAndData()`.** It looked like exactly
this function, and it is not. In Qt 6.4 a *unique* glob match is returned without
the bytes being consulted at all, so a zip called `notes.txt` comes back as
`text/plain` — the one case the second pass exists for. So `identify()` asks the
database twice, once for the magic rules and once for the globs, and chooses:

- A magic match beats the name, unless the name's answer is a **subclass** of it.
  A `.docx` is a zip; answering "zip" would throw away what the name knew.
- When both say text, the name wins. Magic for text formats is thin — an
  `#include` makes any C++ file C — and a wrong language colours a file oddly
  where a wrong viewer shows the wrong thing entirely.
- When no magic rule matches, the bytes still answer *text or not*, and a name
  claiming otherwise loses.

The magic table itself is never reimplemented; only the choice between two of the
database's own answers. A second implementation of the freedesktop precedence
rules would be wrong in ways nobody notices.

**Why a text test of our own.** Qt falls back to `application/octet-stream` when
no magic rule matches and its own heuristic fails, and that heuristic is private,
undocumented and free to change. The rule Mole depends on is written down in
`FileType::looksLikeText()`: a BOM is text, a NUL byte is binary, and more than
2% control characters is binary — counted as C0, or as Latin-1's C1 range when the
sample is not UTF-8, so that a Latin-1 log stays text.

**Why the viewer waits.** The tab shows no viewer while the head is being read,
rather than opening the name's viewer and swapping it. One viewer is created per
file, nothing reads a file it turns out not to be showing, and there is no flicker
from a fact list to a text pane. The cost is that `viewer` is null for the length
of one 4 kB read, which the view says with `identifying` and the tests wait for on
the condition rather than on a clock.

## Consequences

- `FileEntry::mimeType` is now filled by the preview layer, as its comment always
  said it would be, and a backend that knows the type for free still wins: a
  non-empty field means the second pass is skipped entirely.
- Any provider can now claim a file on its contents by reading `entry.mimeType`,
  without doing any I/O and without a new extension point.
- A file whose name got no further than the fallback tier costs one extra read of
  one page. A zero-byte file is not read at all — there is nothing to read, and
  its viewer is the one its name has always chosen.
- `PreviewTabController::viewer()` may be null for a moment after a file is
  opened. Anything that reaches for the viewer immediately after opening a
  preview has to wait for it, and `identifying` is how the difference between
  "not yet" and "nothing can show this" is told.
- What a file *is* is now one answer in one place, so the fact list, the viewer
  choice, and every later reader of a file's details agree. The next tickets in
  this epic — a text viewer of last resort, a hex window, a details panel — all
  start from it.
