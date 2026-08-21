# ADR-0075: A drive offers what only it can do

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

The set of things that can be done to a file was fixed at compile time and
identical on every drive. `IFileSystem` exposes a closed list of operations, and
`VfsCapability` lets a backend say which of *those* it supports. There was no way
for a backend to offer something no other backend has.

Two drives already in the tree have something to offer that nothing above them
could ask for. A local disk on a filesystem that keeps earlier states of a file
knows about those states, and a container that keeps earlier objects under the
same key knows about those; neither could say so. An object store can hand out a
link to an object that stops working after a while, and there was nowhere to put
one.

Adding an enum member per feature was the obvious move and the wrong one. Every
consumer of `capabilities()` is `testFlag(<a name the caller already knows>)` --
`AlertEvaluator`, `QuerySpaceTask`, `QueryAccessTask`, `ReadRangeTask`,
`BrowserFeature`, `BrowserPaneController`, `DriveListModel` and the two metadata
readers. That is correct for what the enum holds: those are capabilities whose
absence changes what the *core* does, so the core has to understand each by name.
A drive-specific feature is the opposite -- only the user acts on it -- so one
enum member per feature would put one `if` in the shell per feature, and require
the interface to know every feature that exists anywhere.

## Decision

**Two tiers, not one.** `VfsCapability` stays exactly as it is and keeps
answering its own question: *does the core have to understand this?* Alongside it,
a drive contributes **actions**, which are the things only the user acts on.

- `FileAction` is a stable namespaced id, a title, and whether it is enabled --
  the shape `MenuAction` already uses, for the reason its header gives: "the
  shell places it without knowing what it does."
- `IFileSystem::actionsFor(uri, entry)` returns what this drive can do to that
  node. It returns nothing by default, so every backend in the tree was already
  correct and none of them was edited.
- `IFileSystem::invoke(id, uri, cancel)` performs one.

**An outcome is one of exactly two things: a piece of text, or a list of
alternate uris for the same file.** Nothing else. Text is shown with a way to
copy it and with whatever the drive said about how long it stays true; a list of
uris is offered as a list to open from, each entry an ordinary file. That is the
whole vocabulary, and it is what lets the interface present the answer to an
action it has never heard of.

**A drive refuses an id it did not hand out**, with `NotSupported`, which is what
the default `invoke()` does for every backend that contributes nothing.

**The conformance suite holds both directions.** A drive that offers an action
must be able to perform it and must answer with an outcome that carries what its
kind promises; a drive that offers none must refuse `invoke()` rather than
answering anyway.

## Reason

**The split is a question, not a list.** *Does the core have to understand this
capability, or only the user?* `ReportsSpace` changes what the drive row draws
and what an alert rule can be written about, so the core has to know the name.
"This container will sign a link" changes nothing above the backend, so nothing
above the backend should learn it. Anything genuinely of the first kind still
belongs in the enum, and the two tiers are not a migration path from one to the
other.

**The closed set of two is the load-bearing part.** An open set -- a variant, a
`QVariant`, a mime type and a blob -- would move the problem rather than solve
it: the shell would branch on what came back, which is the same `if` per feature
in a different place. Two kinds means two branches for ever. A third kind is a
decision to take deliberately, with its own record, not an addition to make in
passing while adding a feature that wants one.

**"It returns a task" is not a third kind.** `IFileSystem` is synchronous by
contract and every call into it already runs on a `TaskManager` worker, so an
action is always invoked as a task; only the answer varies. Putting a task in the
outcome would have made the interface asynchronous in one method and blocking in
the other fourteen.

**`FileAction` lives in `src/core/vfs/` and not in `src/sdk/`**, which is where a
plugin-facing type would normally go and where the task that asked for this
expected it. `IFileSystem` is a core header and returns the type, and the layers
are strictly acyclic with `sdk` depending on `core`; putting it in the sdk would
have inverted that for a header-only struct. Plugins reach it either way -- they
already include `core/vfs/IFileSystem.h` to implement it -- so nothing is lost
but the placement.

**`entry` as well as the uri**, because `actionsFor` is asked about a node the
caller has just listed. Passing what the listing already knows lets a drive rule
an action out -- a directory, an empty file, the wrong suffix -- without a round
trip to answer a question nobody has asked yet.

**Enabled rather than absent.** A drive that cannot perform an action on this
particular file says so with `enabled = false`, so the list of what a drive can
do does not change shape as you move down a folder. It is the same reasoning
`MenuAction::enabled` records.

## Consequences

A backend can offer something nothing else has, and no file above `src/plugins/`
names a filesystem, a storage service or a feature. The first two tenants are
earlier versions of a file, which two shipped backends can answer, and a
time-limited link to an object, which one can; having two unrelated ones is what
proves the slot is general rather than a version viewer with an interface in
front of it.

Every decorator now has two more methods to forward, and forgetting one is
invisible: `LoggingFileSystem` wraps every mount on its way into `VfsManager`, so
a wrapper that did not forward would make the extension point unreachable in the
running application while every test of a bare backend stayed green. Both
decorators in the tree forward, and a test holds the one in the product to it.
The same wrapper still does not forward `nameRules()`, `pathCaseSensitivity()`,
`leftovers()` or `discardLeftover()`, which is a fault of its own and has a task.

Nothing calls either method yet. The interface offering a drive's actions is
MOLE-197, the fake that contributes them without a server is MOLE-196, and
addressing a particular version of a file is MOLE-195.
