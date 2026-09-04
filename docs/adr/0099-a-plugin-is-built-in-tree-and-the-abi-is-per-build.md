# ADR-0099: A plugin is built inside the tree, and the ABI is per build

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

`docs/WRITING_PLUGINS.md` showed a `CMakeLists.txt` for a plugin that links
`mole_sdk`, as though a third party could build one against an installed Mole.
They cannot, and nothing in the build ever suggested they could:

- `mole_sdk` and `mole_core` are `STATIC` libraries that exist only inside a
  configured build tree.
- No header under `src/sdk` or `src/core` is installed. There is no export set,
  no `mole_sdkConfig.cmake`, and no `install(TARGETS mole_sdk …)`. The only
  install rules are `mole`, `mole-tasks` and the two plugin `.so`s.
- Every SDK header needs core headers behind it: `PluginApi.h` reaches
  `IFileSystemFactory.h` and so `IFileSystem.h`; `IPreviewProvider.h` and
  `IMetadataReader.h` need `FileEntry.h` and `VfsTypes.h`; `ScanReaders.h`
  reaches `IndexDatabase.h` and so QtSql. "Build against `src/sdk` alone" is not
  possible even in principle.

So what a third party does today is clone Mole, put their plugin under
`src/plugins/`, and edit a `CMakeLists.txt` — a fork rather than a plugin.

There is a second consequence, and it is the one with teeth. A plugin statically
links `mole_core`, so **every plugin carries its own copy of core**: its own
function-local statics, its own logging category objects, its own registries.
Whether that copy is used depends on whether the host exports its symbols. `mole`
does (`ENABLE_EXPORTS ON`, added for readable backtraces), so the dynamic linker
resolves a plugin's core references to the host's copy and there is one core in
the process. `mole-tasks` did not, so a plugin loaded there ran its own. **Two
hosts, two linking regimes, one plugin binary** — the shape of a fault that
appears in one host and not the other with nothing to explain it.

## Decision

**Plugins are built in the tree, and this is written down rather than implied.**
`WRITING_PLUGINS.md` says so in its first paragraph, `ARCHITECTURE.md` says it
where it describes the plugin host, and the guide's CMake sample says where the
file it shows belongs.

**The ABI is per build.** A plugin belongs to the tree it was built in. The
version check (ADR-0098) refuses one built against a different API version, which
is the coarse half of the same statement.

**Both hosts export their symbols.** `ENABLE_EXPORTS ON` is set on `mole-tasks`
as well as on `mole`, so a plugin's core references resolve to the host's copy in
either. The dependence is recorded beside the property in
`src/tools/CMakeLists.txt`, because it is not obvious from either file that a
linker setting is what makes a plugin's statics singular.

## Reason

The alternative is to make the SDK installable, and it is a real piece of work
rather than a few install rules:

- **Which core headers are contract?** `IFileSystem`, `FileEntry`, `VfsUri`,
  `VfsTypes`, `Result`, `CancelToken`, `Task`, `TaskManager`, `IndexDatabase`
  and what they include in turn. Installing them makes each one a published
  surface that cannot then be reshaped without a version bump — which is a
  promise this project is not ready to make about `src/core`, where the shape of
  things is still moving several times a month.
- **A static core cannot be a shared ABI.** Installing static libraries and
  exporting targets would let a plugin build outside the tree and would leave
  every plugin carrying its own core, relying on the host's exported symbols
  interposing it. That works today by accident and would then be load-bearing.
  Making `mole_core` shared is the honest form of it — and that is symbol
  visibility, install rules, an soname policy and a look at every static in the
  library.
- **There is no third-party plugin.** The two shipped ones live in the tree, and
  the archive plugin exists mainly to prove the published API is sufficient to
  add a drive from outside the core. Nothing is waiting on this.

So the decision is to be accurate about what exists rather than to build a
distribution surface nobody can use yet. When a third-party plugin is actually
wanted, the order is: make `mole_core` shared, decide the contract headers,
install them with an export set, and write the ADR that supersedes this one.

Rejected outright: **installing the headers without an export set or a shared
core**, which is the worst of both. It would make the guide's sample compile and
leave the resulting plugin's behaviour dependent on a linker property of the host
that loaded it.

## Consequences

Somebody who wants to write a plugin has to clone Mole, and the guide tells them
so in its first sentence rather than after an afternoon of CMake.

The two hosts now behave the same way about a plugin's core, which is what
`tst_MoleTasks` and the plugin suites were quietly assuming.

`kPluginApiVersion` remains the coarse guard it was designed to be: it refuses a
plugin from another build's SDK, and per-build ABI means that is the right
answer rather than a limitation.
