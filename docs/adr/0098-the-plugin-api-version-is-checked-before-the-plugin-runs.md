# ADR-0098: The plugin API version is checked before the plugin runs, and what may change without a bump

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

`PluginManager::acceptPlugin()` obtained the version that decides whether a
plugin may be spoken to **by speaking to it**. `plugin->metadata()` is a virtual
call through the plugin's own vtable, returning `PluginMetadata` by value, and
`apiVersion` was the last field of that struct after five `QString`s.

Two things follow, and neither is theoretical:

- If `IPlugin` ever gains a virtual before `metadata()`, an old plugin's vtable
  has a different function at that slot. The host calls it and finds out what
  happens.
- If `PluginMetadata` gains a field before `apiVersion` — and the natural append
  is another `QString` after `description` — an old plugin returns a struct laid
  out differently from the one the host reads. The version check then compares
  whatever landed at that offset, which on a 64-bit machine is half a pointer.

The two mechanisms built for exactly this were idle. `MOLE_PLUGIN_IID` was
`"io.github.ogefest.mole.Plugin/1.0"` while `kPluginApiVersion` went 8, 9, 10,
11 — so `qobject_cast<IPlugin*>` accepted a plugin from any of those versions.
And `QPluginLoader::metaData()`, which is readable *before* `instance()`
constructs anything, was never consulted; since daa6131 the JSON manifests held
no version to consult either. `docs/WRITING_PLUGINS.md` rule 4 promised "a
mismatch is refused at load with a clear message instead of crashing later",
which held only while two struct prefixes never changed, and nothing pinned them.

The second half is `PluginServices`. Its own comment says fields "may be
appended, but never removed or reordered without bumping kPluginApiVersion", and
ADR-0066 treats appending as safe. It gained `indexSummary` on 2026-08-21 and
`chains` on 2026-08-23 with no bump — and `IMetadataReader::read()` and
`IThumbnailer::thumbnail()`, both implemented by the plugin, took it **by
value**. A plugin built at version 11 on 2026-08-20 has a struct two pointers
shorter than the host's; it passed the version check and was handed a
fourteen-pointer copy where it expected twelve. That works, but only because on
every current ABI a trivially copyable struct of this size is passed as a memory
copy and it is the sole such argument — an accident of the signature rather than
a property of "append-only".

## Decision

**The version is in the interface identifier, and it is checked from the
library's metadata before anything in the library runs.**

`MOLE_PLUGIN_API_VERSION` is a macro, so the same number is both compared as an
integer and pasted into `MOLE_PLUGIN_IID`
(`io.github.ogefest.mole.Plugin/12`). `PluginManager::loadFromDirectory()` reads
`loader->metaData()["IID"]` and refuses a mismatch there — before `instance()`,
so before the plugin's constructor and long before `metadata()`. A refusal says
which version the library was built against and which this host provides.

**The in-object check stays, as a second line.** It catches a built-in, which has
no metadata to read, and a plugin whose `metadata()` says something other than
what it was compiled against.

**`apiVersion` is the first field of `PluginMetadata`.** The host reads that
field out of a struct the plugin returned, so the two must agree about where it
is before they can agree about anything else. First, an append can never move it.

**`PluginServices` is passed to plugin-implemented virtuals by `const&`.** The
host owns it and it outlives every call. This is what makes appending honestly
ABI-neutral: the plugin reads the host's own object through its own shorter view,
and the prefix of a struct is the same struct. So the rule the header states is
now the rule the ABI keeps:

- **Appending a field to `PluginServices` needs no version bump.**
- **Reordering or removing one does**, and so does changing the meaning of a
  field, adding a virtual anywhere in an interface, or changing any signature.

## Reason

The alternatives considered:

- **Put the version in the JSON manifest** and check that instead. It is a real
  option — `metaData()["MetaData"]` is the manifest, readable at the same moment
  — and it was rejected as a second place the number lives. The identifier is
  generated from `MOLE_PLUGIN_API_VERSION` by the preprocessor, so it cannot
  disagree with the constant; a hand-written `"apiVersion": 12` in two manifests
  can, and would be the first thing to go stale.
- **Keep the in-object check as the only one, and freeze the two prefixes** —
  document that `IPlugin::metadata()` must stay the first virtual and
  `apiVersion` the first field, and rely on that. Rejected: it is a rule with
  nothing enforcing it, protecting a mechanism whose whole job is to protect
  against that class of mistake.
- **Bump the version on every change to `PluginServices` and drop the
  append-only note.** Simpler to reason about, and rejected because the version
  is the thing that refuses a plugin outright: a bump for an appended field means
  every plugin stops loading for a change that costs them nothing. The
  append-only promise is worth keeping — it just has to be true.
- **Give `PluginServices` an explicit size or version field** and have the plugin
  check it. That is the C way and it works, and it puts a check in every plugin
  where a reference removes the need for one.

## Consequences

`kPluginApiVersion` is 12, and every plugin has to be rebuilt — which is the
point of a version, and there is no third-party plugin yet to inconvenience.

Two fixture plugins now exist in `tests/support/plugins/`, built as real shared
libraries because the questions cannot be asked in-process: one declares
`…Plugin/1` and **aborts if it is asked what it is**, so the test reaching its
assertions is the assertion; the other is compiled with a `PluginServices` two
fields shorter and reports the address it read for every field it knows, which
the test compares with what it handed the host. A reordering of the struct fails
that comparison, which is the check the append-only note never had.

An IID that carries a number means a plugin author sees
`io.github.ogefest.mole.Plugin/12` in their own source through `MOLE_PLUGIN_IID`
and must not write it out by hand. The macro is the only supported spelling, and
the two shipped plugins use it.
