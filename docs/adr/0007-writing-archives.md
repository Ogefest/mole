# ADR-0007: Compression writes new archives with libarchive, in three formats

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

Packing files is one of the things a file manager is for, and there was no way to do
it. libarchive was already a dependency — the archive *reading* backend uses it to
browse inside zip, tar and 7z — and it writes as well, so nothing new had to be
taken on.

Three decisions were needed: which formats to offer, where the code lives given that
libarchive is optional, and what happens to the archive mounts that are read-only
today.

## Decision

**Five formats: zip, tar.gz, tar.xz, 7z and a bare xz.** Zip is the default because
it is the one anyone can open anywhere, on any platform, without being told how.

**7z is offered** (this supersedes the first version of this decision, which excluded
it on the grounds that libarchive writes less of the format than it reads). Measured
on libarchive 3.7.2: a multi-entry 7z writes and reads back correctly. What it cannot
do is encrypt — see below.

**A bare `.xz` is one compressed stream with no container**, so it holds exactly one
file and no folders. Asked for with several items or a folder, the operation refuses
before writing a byte rather than failing on the second entry with *"Raw format only
supports one entry per archive"*, which is true and useless.

**The task lives with the backend that already links libarchive.** `CompressTask`
is part of `mole_archive_backend`, and `mole_builtin` links that library when it
exists, defining `MOLE_HAVE_ARCHIVE`. Without libarchive the action is not
registered at all, so it cannot be offered and then fail.

**It writes a new archive and nothing else.** Archive mounts stay read-only.

**A password is offered only where the format can carry one: zip**, encrypted with
AES-256. A passphrase given for any other format is *refused* rather than ignored,
including 7z: handing back an archive anybody can open to someone who typed a password
is the worst answer available.

7z deserves its own note, because it hides a trap. This libarchive rejects
`7zip:encryption` as an undefined option — but it accepts a passphrase anyway and
returns success, and the written file contains no plain text, because LZMA2 compressed
it rather than anything encrypting it. A test that looked only for the plain bytes
would report encryption working. Whether a format can carry a password is therefore a
stated fact about the format, never an inference from the output.

**Both ends are drives.** Sources are read through `IFileSystem` and the archive is
written through it too, so packing a selection on a remote drive is the same code as
packing one on local disk.

## Reason

Compression is an operation on the files in front of you, not a kind of drive, so it
belongs with the built-in operations rather than inside the loadable archive plugin
— which exists to publish a filesystem. That is why `mole_builtin` grows the link
rather than the plugin growing an action: the menu entry is the shell's, the
libarchive call is the backend's, and the optional dependency is handled the same way
Qt PDF is for previews.

Adding to an existing archive was considered and rejected for now. It sounds like a
small extension of writing one and is not: it means rewriting the container, deciding
what happens to duplicate paths, and having something sensible to do when the process
dies half way. Writing a new archive has one failure mode — the output is incomplete —
and one obvious remedy.

## Consequences

- A cancelled or failed compression deletes the partial archive. An archive that
  exists is one that finished; there is no state where a half-written file waits to be
  mistaken for a good one.
- The format is chosen per operation rather than remembered, because unlike how a
  viewer shows a file, what to pack something as depends on who is going to open it.
  The same goes for a password, which is never stored anywhere.
- AES-256 rather than zip's original encryption, which is broken and known to be. A
  build of libarchive that cannot do it fails the operation rather than falling back to
  the weak scheme.
- What is packed follows the rule every other operation here follows: the ticked
  entries, or the row under the cursor when nothing is ticked, and the folder in view
  only when there is no row at all. The dialog names it before anything happens, so
  the rule is never something to be guessed at.
- The test packs a tree and then reads it back through the archive *reading* backend,
  so the writer and the reader check each other rather than the writer being checked
  against its own idea of what it wrote.
- In a build without libarchive there is no Compress entry, and the archive plugin is
  not built either, so a `.zip` is a file rather than a drive. Both absences have the
  same cause and are reported by the same configure-time message.
