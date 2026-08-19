# Architecture

Mole is built around three extension points. Almost everything the
application does is reached through one of them, including the parts that ship
in the box.

| Extension point | Adds | Interface |
|---|---|---|
| Filesystem backend | a new kind of **drive** (SFTP, S3, a git forge, an archive) | `IFileSystemFactory` + `IFileSystem` |
| Feature | a new kind of **tab** (browse, search, duplicates, analytics) | `IFeature` + `FeatureController` |
| Preview provider | a new kind of **viewer** (text, PDF, SQLite, Parquet) | `IPreviewProvider` + `PreviewController` |
| Menu action | an entry under **File / View / Tools / Help** | `MenuAction` |

Every tab kind also decides what it wants remembered between runs, through
`FeatureController::saveState()` / `restoreState()`.

## Layers

Strictly acyclic, each depending only on the ones above it:

```
core     VFS, tasks, event bus, index.  Links QtCore + QtSql only.
  ↓
sdk      The published plugin API.  What third-party code is allowed to see.
  ↓
host     Registries plugins write into, plus the loader that finds them.
  ↓
ui       List models and controllers.  No QML, no type registration.
  ↓
builtin  The features that ship in the box, registered like any plugin.
  ↓
app      main() and the QML shell.
```

`core`, `sdk`, `host` and `ui` are all headless, which is why the entire test
suite runs without a display and finishes in under a second.

## The threading rule

**The UI thread never touches storage.**

`IFileSystem` is deliberately synchronous and blocking. Every call is made from
a `TaskManager` worker thread; the UI submits a `Task` and waits for a signal.
That single rule is what keeps a stalled NFS mount from freezing the window,
and it means a new backend can be written in plain blocking style with no async
plumbing at all.

`Task` handles the awkward half: `run()` executes on a pool thread, while the
`Task` object itself lives on the UI thread and marshals progress back through
queued invocations. Subclasses touch only their own private data inside `run()`.

Two consequences worth remembering:

- `QSqlDatabase` connections belong to the thread that opened them, so
  `IndexDatabase` hands out one connection per calling thread and relies on
  SQLite's WAL mode for the overlap.
- Cancellation is cooperative. A long operation must poll its `CancelToken`,
  and every backend is held to that by the conformance suite.

## VFS

A `VfsUri` addresses any node anywhere: `scheme://authority/path`. A `Mount`
binds a configured backend to a root uri and a display name — the sidebar shows
local disks, network shares and opened archives as the same kind of row, on
purpose. `VfsManager::resolve()` picks the backend for a uri, longest matching
root first, so nesting one drive inside another works.

Backends never see each other. Anything shared — recursive traversal,
cancellation, unreadable directories — lives in `DirectoryWalker`.

### Mountable files

`IFileSystemFactory::mountableFileSuffixes()` is how activating a `.zip` in the
browser turns it into a drive. The host does not know what an archive is; it
asks the registered factories which suffixes they can mount. A future backend
that mounts `.iso` or opens a `.sqlite` as a table drive works the same way with
no change anywhere in the shell.

### Capacity

`IFileSystem::space()` is optional and defaults to `NotSupported`. Only backends
advertising `VfsCapability::ReportsSpace` are asked, and only the local disk does
today. A bucket has no capacity in any useful sense and an archive's "size" is
the file it came from, so the sidebar draws nothing for them rather than a bar
built on an invented number — a chart is read as a fact.

The query goes through `QuerySpaceTask` like everything else that touches
storage: `QStorageInfo` blocks on an unreachable NFS mount, and asking from the
UI thread would freeze the window. It is marked background so a refresh every
minute cannot bury the copies and scans the user actually started.

## Tabs

`TabsModel` holds `(feature id, controller)` pairs. The shell renders whatever
`IFeature::viewSource()` points at and injects the controller as a `controller`
property. It never learns a browser from a duplicate finder.

A `FeatureController` owns its tab's label, so a browser tab renames itself to
the folder it is showing without the shell knowing what a folder is.

## The menu

One hamburger, four sections fixed by `MenuAction::Section`. Entries come from
the shell and from plugins through the same `PluginRegistry::addMenuAction()`,
and the "New … tab" entries are generated from the feature registry — so a
plugin that adds a tab kind appears in the menu without the shell being edited.

`ActionRegistry::buildModel()` rebuilds the whole thing on every open rather
than keeping it live. Menus are read rarely, re-evaluating a handful of
predicates costs nothing, and it removes a class of stale-tick-box bugs.

View entries reach the current tab through `QObject::property()` rather than a
cast, so they keep working for tab kinds the shell has never heard of: a tab
that does not answer simply greys the entry out.

## Sessions

Closing the application and reopening it puts the user back where they were.
`TabsModel::captureSession()` asks each open tab what it wants remembered;
`SessionStore` writes that to JSON. The shell never interprets the contents --
a tab's state map is entirely its own, so a plugin's tab persists without the
shell learning anything about it.

Three rules the format follows, because a session file is read at startup and
a bad one must never stop the application launching:

- Writes go through `QSaveFile`, so a crash mid-write leaves the previous
  session intact rather than a truncated file.
- A missing, corrupt or newer-versioned file yields an empty session and a
  normal fresh start.
- A tab whose feature is no longer registered -- an uninstalled plugin -- is
  skipped; the rest still come back.

Saving is debounced: navigation reports a state change constantly, and the file
is not worth rewriting per keystroke. The destructor flushes whatever the
debounce had pending.

## Previews

`F3` opens a tab that shows one file. The tab owns two things: which file is
current, and the list of its neighbours so the arrows can step through the
folder. Everything about *how* a file looks comes from an `IPreviewProvider`,
chosen by priority — a plugin adding a viewer touches nothing in the tab.

Four ship in the box: table (CSV/TSV), image, text (with JSON and XML
coloured), and a file-information fallback that accepts everything at the
lowest priority. That last one is why "nothing happens" is never the answer to
F3; an unrecognised file gets described instead of ignored.

Two rules the providers follow:

- `canPreview()` does no I/O. It sees a name, a suffix and a size, so choosing
  a viewer never blocks.
- A provider claims only what it can actually render. The image viewer asks
  `QImageReader` which formats this build supports rather than hard-coding a
  list, so an unsupported format falls through to something that can say
  something useful.

## Filtering versus searching

Two different things that look alike, kept apart on purpose:

- **Filter** (`FileListModel::setFilterText`) hides rows that are already
  loaded. No traversal, no I/O, no task — it cannot be slow, so it lives in a
  bar rather than a tab and answers as you type.
- **Search** walks a tree (or queries the index) on a worker thread, streams
  results in, and can be cancelled. It gets a tab because it takes time and
  produces its own list.

Giving the cheap one the shortcut people press without thinking (`Ctrl+F`) is
deliberate.

## File operations

`TransferTask` copies and moves through `IFileSystem`, so local-to-local and
NAS-to-S3 are the same code path. Two rules it will not bend:

- A move within one backend is short-circuited to `rename()`. Never stream
  bytes when the filesystem can just relabel them.
- A move deletes the source only when *every* entry arrived. Losing data to
  tidy up after a half-failed copy is not a trade worth making.

Directories are expanded through `DirectoryWalker` into a flat job list, parent
first, so children always have somewhere to land. An existing target directory
is a merge, not a conflict.

## Events

`EventBus` is the decoupling layer. A scan finishing announces
`indexUpdated`; every open indexed-search tab refreshes, not just the one that
started it. A created file announces `entryCreated` *and* `directoryChanged`,
so open panes reload without polling.

`post*()` is safe from any thread and always delivers on the bus's own thread —
same-thread posts are synchronous so the UI does not lag a frame behind itself.
Plugins get `postCustom(topic, payload)` with namespaced topics.

## Index

`IndexDatabase` is a SQLite catalogue of previously scanned trees. `ScanTask`
walks a root and records it; `IndexSearchTask` queries it. That is what makes
"find that file on the 4 TB NAS" instant rather than a network traversal, and
it is why live search and indexed search are separate tabs: one is always
current, the other is always fast.

Names are stored twice — as written and Unicode-lowercased — because SQLite's
own `LIKE` and `NOCASE` only fold ASCII, so "Łódź" would never match "łódź".

Schema changes are append-only migrations keyed off `PRAGMA user_version`.

## Testing

- Every backend must pass `runFileSystemConformance()`. When SFTP arrives, its
  test file is a few lines that build a context and call it. If it disagrees
  with the local disk about what `NotFound` means, that fails before any UI
  code sees it.
- `MemoryFileSystem` is both a real scratch drive and the main test double. Its
  `setFault()` and `setListDelayMs()` reproduce "the NAS went away mid-scan" and
  "this mount is slow" without needing a NAS.
- `make asan` runs everything under AddressSanitizer, UBSan and leak detection.
- `tst_AppIntegration` assembles the application the way `main()` does, minus
  the QML engine, and opens every registered feature.

### Testing the interface

`QmlAppHarness` builds the whole application — plugins, tabs, real `Main.qml` —
on the offscreen platform, and drives it with `QTest::keyClick` posted straight
to the `QQuickWindow`.

This replaced an Xvfb-and-`xdotool` harness that was worse than no harness. With
no window manager running there is no X input focus, so synthetic keystrokes
went nowhere: roughly two of every six arrived. Tests went green because nothing
happened. It cost four rounds on one Enter-key bug — a real defect that looked
like a harness fault, next to a harness fault that looked like a real defect.

Posting to the window uses the same delivery path the application sees in
production, minus the display, so there is no focus lottery to lose. Screenshots
come from `QQuickWindow::grabWindow()`, which means `make screenshots` cannot
produce a picture of a state the assertions did not just verify — every image
under `build/debug/screenshots/` is a passing test, photographed.

`tst_Walkthrough` is that suite: browsing, previewing, filtering by typing, dual
pane and grid, analysing a folder, scheduling it, and the empty window.

## Automation

`core/automation` runs jobs on a clock. A `ScheduleRule` is a job kind, its
parameters, an interval and when it last ran; `Scheduler` polls, decides what is
due, and hands it to whoever registered that kind. It knows nothing about
reports — the analysis plugin registers `AnalysisJob` for the `"analysis"` kind,
and any plugin can register its own.

Polling rather than one timer per rule, because a laptop asleep for two days has
to notice on waking, and a wall-clock timer armed before the sleep would not
fire. A rule that has never run is due immediately, so a job whose turn came
while the application was closed runs at the next start instead of waiting out
another full interval.

Three states the design deliberately makes visible rather than silent:

- A rule whose plugin is gone is recorded as `Skipped` with the reason. A job
  that quietly never runs is the one failure nobody can diagnose.
- A run interrupted by a crash or a quit is reloaded as `Failed`, not `Running`.
  Left as `Running`, every future poll would skip it forever.
- Consecutive failures are counted, so a job failing every night ranks above one
  that failed once. The tracking tab sorts broken rules first for the same
  reason: the reason to open it is that something stopped working.

Scheduling is offered on the report itself rather than in a settings screen, one
rule per folder keyed off the folder, and the clock starts from the report that
already exists so analysing by hand does not trigger a second walk a second
later.

## Acting on a list of things

Copy, rename, analyse, sync and the rest all need the same thing: what to act on.
They take it as a list of uris, and the shell asks the current tab for it by
name — `targetUris()` — rather than by type.

That is why a file set costs nothing. It answers the same question a pane's
selection answers, so every operation written before sets existed takes one
without a line changing. The alternative, discovered the hard way in plenty of
file managers, is that each operation grows a second code path for sets, then a
third for search results, and adding a fourth kind of source becomes a project.

The rule to keep: a new kind of thing that can be acted on implements
`targetUris()`. It never gets its own accessor.

## Virtual drives

A drive is a plugin. `IFileSystemFactory` has always been the seam; it now also
carries the *form*: each backend declares its fields — name, help, whether it is
a password, whether it is advanced, which provider it applies to — and the
configuration dialog is built from that. Adding a backend means implementing the
interface. It never means touching the interface layer.

The network drives are the worked example: **SFTP, FTP, S3, WebDAV, SMB and NFS**,
in `src/plugins/network/`, built as the loadable `mole_plugin_network` rather than
compiled into the application. They replaced rclone, which bought forty providers
for 115 MB of Go and a form nobody could fill in —
[ADR-0011](docs/adr/0011-network-drives-without-rclone.md) records why that trade
was reversed.

The first four sit on one dependency, **libcurl**, which speaks sftp, ftp, ftps and
https between them. That choice follows from the threading contract above rather
than from a feature list: `IFileSystem` is synchronous and called only on worker
threads, and libcurl's easy interface is blocking. `QNetworkAccessManager` would
have needed an event loop pumped on every worker — async plumbing in the one place
the design went out of its way to forbid it.

A curl handle is not thread-safe and `IFileSystem` must tolerate concurrent calls,
so each backend keeps a small pool and lends one out per call. That satisfies the
contract and keeps libcurl's connection cache, which is what stops an SFTP drive
renegotiating SSH for every listing.

SMB and NFS each bring a library of their own — Samba's `libsmbclient` and `libnfs`
— and each is optional at configure time, so a machine without one gets the other
drives rather than a build failure. Neither library is thread-safe in the way
libcurl is not, and the two answers are different because the libraries are: SMB
serialises the whole process behind one session, because its `smbc_*` wrappers act
on a global context and calling round them corrupts Samba's allocator
([ADR-0048](docs/adr/0048-windows-shares-through-libsmbclient.md)); NFS leases a
mounted context from a small pool, because a libnfs context is self-contained and a
file handle belongs to the context it was opened on
([ADR-0050](docs/adr/0050-nfs-through-libnfs-and-a-leased-mount.md)).

S3 request signing (AWS SigV4) is implemented here, in about a hundred and fifty
lines of HMAC-SHA256 over the OpenSSL that the credential store already required.
The signer owns the encoding of the path and the query, and the url that is sent is
built from the same encoder — a request signed for one path and sent to another is
the classic way this goes wrong. It is checked against curl's own `--aws-sigv4` for
a set of request shapes, because a signing bug is otherwise indistinguishable from
a wrong password.

There is no FUSE anywhere. A drive is virtual and lives inside the application; it
is never a mount the rest of the system can see. That is one code path on every
platform, and it is why there is no SSHFS backend — SFTP is the same protocol
without the kernel.

### Credentials

Two requirements pull against each other, and the resolution is the design.

"Not plain text" usually means the system keyring. But a keyring is tied to the
login it belongs to, so a keyring-backed secret does *not* survive reinstalling
the operating system however carefully the configuration was backed up — which is
the point of a keyring and the opposite of what is wanted.

So the key comes from a passphrase the user carries. Nothing in the file depends
on this machine: copy it to a fresh install, type the passphrase, and the
credentials are there. scrypt for the derivation, because a passphrase is
low-entropy and a memory-hard function is what makes guessing it expensive;
AES-256-GCM for the contents, with the header authenticated too — otherwise an
attacker could weaken the cost parameters and the file would accept the result.

The split is enforced rather than remembered. `drives.json` holds settings and
the *names* of fields that have secrets; the values are only ever in the
encrypted store. A save that would need to write a secret while the store is
locked fails rather than degrading, and a config assembled without one comes back
empty rather than partial — connecting with a blank password fails in a way that
looks like a wrong one, and on some backends succeeds as an anonymous user.

## Optional dependencies

Three features depend on libraries that may not be installed, and all three
follow the same shape: found at configure time, compiled behind a define, and
reporting themselves unavailable at runtime rather than failing to build.

- **Parquet** needs Arrow. Without it `ParquetTable::isSupported()` is false, the
  provider declines the file, and it falls through to the information viewer.
- **The terminal** prefers libvterm. Without it a built-in parser handles
  line-oriented output and the panel says "basic mode" instead of drawing a
  full-screen program wrongly.
- **Network drives** need libcurl and OpenSSL. Without either, the network plugin
  is not built and those drives are absent from the list rather than offered and
  failing — the same pattern as libarchive for archives.
- **Credential encryption** needs OpenSSL. Without it the store refuses to hold
  anything rather than falling back to writing secrets in the clear.
- **Everything else** is Qt, which is not optional.

Arrow's headers must come before any Qt header: Arrow declares a parameter named
`signals`, and Qt's macro of that name expands to `public:`.

## Deliberate gaps

- Writing into archives. It means rewriting the container — a different feature
  with different failure modes.
- FUSE. Mounting into the kernel does not port to Windows, and the internal
  abstraction already gives identical operations on every drive.
- Full-text search over the index. `instr()` over an indexed column is fine at
  the current scale; FTS5 is the upgrade path and needs no API change.
