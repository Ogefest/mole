# ADR-0101: A plugin says what it can pack, and the shell asks

- **Date:** 2026-09-05
- **Status:** Accepted. Extends
  [ADR-0007](0007-writing-archives.md), which stands: an archive is written
  rather than modified, and this is about who is asked.

## Context

`AppController` carried seven members behind `#ifdef MOLE_HAVE_ARCHIVE` —
`canCompress()`, `compressionFormats()`, `formatSupportsPassword()`,
`formatTakesOneFileOnly()`, `suggestedArchiveName()`, `archiveNameForFormat()`
and `compressSelection()` — and every one of them called a static of
`CompressTask`, which belongs to the archive plugin. `mole_ui` linked the
plugin's backend and was compiled with a define that said whether libarchive had
been installed when the build was configured.

So `src/ui` knew a plugin by name, knew its format table, and could not be built
without deciding about one of its libraries. Every other contribution — drives,
tabs, viewers, metadata readers, thumbnailers, menu entries — reaches the shell
through the SDK, and this was the one that did not. It was left behind by
MOLE-395, which cleaned up the rest of `AppController`, because it is an
extension point rather than a move: the compress dialog is QML the shell owns,
and it asks these questions *as it is being filled in*.

The questions are real ones and only the writer can answer them. Which kinds
exist. What each is called. Whether a passphrase means anything — only zip
carries one, and a box offered for a tar would be quietly ignored. Whether the
kind can hold more than one file — a bare `.xz` is one compressed stream, and
finding that out halfway through writing is too late to tell anybody.

## Decision

**The plugin answers the questions; the dialog stays the shell's.**

The SDK gains `IArchiver`, a seventh contribution on `PluginRegistry`:

- `formats()` — every kind this can write, each carrying its id, its suffix,
  whether it takes a password and whether it holds one file only. The id is what
  the picker shows *and* what it hands back, so it is `zip` and `tar.gz` rather
  than an internal code.
- `compress(Request)` — the packing, started in the background on the plugin's
  own task manager. The plugin resolves the drives at both ends and posts what
  changed on the event bus, because what was removed is something only the task
  knows.

`ArchiveRegistry` in `src/host` holds what is registered, aggregates the formats
in registration order, and hands a request to whichever archiver claims its kind.
Two archivers claiming one kind is refused at registration: "which one packs
this" decided by load order is not an answer anybody could predict.

The shell keeps what is its own — the base name, which is about the selection and
the folder in view — and takes the suffix from the format. `MOLE_HAVE_ARCHIVE`
appears nowhere under `src/ui`, `CompressTask` is not spelled there, and
`mole_ui` no longer links the backend.

`kPluginApiVersion` goes from 12 to 13 and the IID with it, because a pure
virtual was added to `PluginRegistry` and every plugin built against 12 has a
copy of the old vtable's shape.
[ADR-0098](0098-the-plugin-abi-is-one-number-and-the-loader-reads-it-first.md)
is what makes that one honest step: the number is pasted into the interface
identifier, so a plugin built against 12 is refused before its constructor runs
rather than crashing inside it.

## Reason

**A plugin owning the whole dialog** is the cleaner boundary — the plugin knows
what it needs to ask — but it needs a route for a dialog the shell does not know:
`dialogRequested` names a dialog the shell has QML for. That is a larger change
to how dialogs are opened than this fault earns, and it would have to be designed
for every plugin rather than for this one.

**Leaving the coupling and recording it as an accepted exception** is the
cheapest answer and was rejected for what it closes off: no second plugin could
ever add a format, because the list is a static in another plugin's class. An
extension point that admits exactly one implementation is not one.

**A capability query returning a map** — `describe()` answering a `QVariantMap`,
`invoke(verb, arguments)` — would need no new type per capability, and was
dropped because it is untyped at the one place typing pays: a plugin that spells
`takesPassword` as `takes_password` compiles, loads, and quietly offers a
password box that is ignored. Every other extension point here is a typed
interface, and this is not the one to make an exception of.

## Consequences

- What a plugin author must do to add an archive format: implement `IArchiver`
  and register it. Nothing in the shell changes, and the compress dialog offers
  the new kind with the right password box and the right warning.
- A build without libarchive builds no archive plugin, so nothing registers an
  archiver: `canCompress()` is false, the operation is not offered, and asking
  for it anyway says "This build was made without libarchive" as it always did.
  It is a run-time fact now rather than a compile-time one, which is why the
  shell no longer has to be compiled twice.
- `mole_builtin` links the archive backend for its own reason — the document
  metadata reader opens `.docx` and `.odt`, which are zip containers — and now
  says so in its own CMake rather than inheriting it from `mole_ui`.
- Every plugin must be rebuilt, which is what a version bump means here: they are
  built in this tree against these headers
  ([ADR-0099](0099-a-plugin-is-built-in-tree-and-the-abi-is-per-build.md)).
