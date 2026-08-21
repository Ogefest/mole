# ADR-0076: A drive is asked what it can do when somebody looks at it

- **Date:** 2026-08-22
- **Status:** Accepted

## Context

ADR-0075 gave a drive somewhere to put what only it can do. It did not say how a
drive finds out, and for the two things that tier exists for, the drive cannot
know from its own code.

`LocalFileSystem::capabilities()` returns a literal — the same answer on every
filesystem it is ever given. `S3FileSystem::capabilities()` does the same for
every container. That is right for what the enum holds: `Write` and
`RandomAccessRead` really are properties of the code. It is wrong for anything
that depends on **what the drive was pointed at**. The same local backend has
earlier states of a file on one filesystem and not on another; the same
object-store backend has them on one container and not on another. One class,
two answers.

So the answer has to be discovered from the drive. The question is when.

## Decision

**On first need, and cached for the life of the mount.**

- `IFileSystem::offers()` reports what has been discovered and never asks. It is
  cheap, non-blocking and safe from the thread that draws.
- `IFileSystem::probe(target, cancel)` finds out, once, from a worker thread. The
  second call and every one after it cost nothing, and a call arriving while
  another thread is asking returns rather than asking again or waiting.
- A backend overrides `askWhatIsOffered()` and nothing else. The once-ness, the
  state and the failure handling are the base class's.
- `ProbeDriveTask` is what calls it, submitted by whoever opens a folder —
  background, one of many, and never mentioned by the task strip.
- **The answer has three states, not two.** `Unasked`, `Answered` and `Failed`.
- **A probe that fails leaves the drive working**, with the offers absent and a
  line in the log under the `drive` subject. It is never reported to whoever
  opened the folder: they asked for a listing.

## Reason

**Not at mount.** Mounting must not get slower, or fail, because of a capability
nobody has asked for yet. That was settled before this was written.

**Not when a drive is configured either**, recorded once into its definition, and
there are two reasons. *The drive where this matters most is never configured*:
local volumes are not defined but discovered — `SystemVolumes::enumerate()` walks
`QStorageInfo::mountedVolumes()`, so a disk that is plugged in simply appears, and
there is no moment to hang a stored answer on. `RemoteRegistry` does have one for
remote drives, but the local case has none, and that is exactly where a
filesystem keeping earlier states of a file lives. *And a stored answer goes
stale silently, a stored "no" being the worst kind*: whether a container keeps
earlier objects, or a volume is being snapshotted, is a setting somebody changes,
and Mole would go on saying nothing for ever with nothing on screen to explain
why. This is not hypothetical — there are no snapshots on the development machine
today, so a probe run now would record "no versions here" and be wrong the first
day anybody takes one.

**On first need is less work than up front, not more.** Open Mole with eight
drives configured and browse one, and there is one probe rather than eight; a
drive nobody opens costs nothing, ever. The costs even fall the convenient way
round — the drives whose probe is a network call are the ones you have to open
before you can care, and the drives with no definition moment are the ones whose
probe is a `stat` and free.

**Its own task, not a step inside `ListDirectoryTask`.** The probe rides along
with work that is happening anyway, but it must not be *inside* it: a probe that
never comes back would otherwise be a folder that never opens, and a capability
nobody asked about would be able to break browsing. The listing is answered
without waiting for it, and the test that holds this waits for the probe to be in
flight rather than for a clock.

**Three states, because absent and not-yet-known are different and only one of
them is a lie.** Before the answer arrives the drive has not said it cannot do
the thing; it has said nothing. Anything drawing "no" out of that silence tells
the user something no drive ever said — and then has to un-say it a moment later,
which is a flicker as well as a lie. `Failed` is a third rather than folded into
either: it is absent like an empty answer, but for a reason worth telling apart
from one, and it is the state a retry would make sense from.

**A cancelled probe leaves the drive `Unasked`.** It found nothing out, so the
next folder opened there asks again. Recording it as `Failed` would turn a user
navigating away into a drive that says nothing for the rest of the session.

**The state lives on the backend instance**, which is what "for the life of the
mount" means: `VfsManager` builds one backend per mount and lets go of it when
the drive goes away, so a drive unmounted and mounted again is asked again. It
has to be on the drive rather than in the manager because the backend itself is
the first consumer — `actionsFor()` has to know what the probe found without
asking the layer above it, which it must not depend on.

**The guard is in two places on purpose.** The drive refuses to be asked twice,
and `BrowserPaneController` does not submit a task for a drive that has already
answered. Without the second, browsing would queue a job per navigation that does
nothing; without the first, two panes opening at once would both ask.

## Consequences

`VfsCapability` keeps answering its own question and gains nothing. Its comment
now says where a target-dependent capability goes instead, because the obvious
move — one more flag — is the one to talk somebody out of.

Every decorator has two more methods to forward. `LoggingFileSystem` and
`FaultyFileSystem` do, and a test holds the first to it, for the reason ADR-0075
already recorded: the wrapper is on every mount, so a wrapper that kept an answer
of its own would be a second drive that can disagree with the first.

`MemoryFileSystem` can be pointed at either kind of drive — one that offers
something, one that offers nothing, one that cannot say, and one that takes for
ever to answer — so everything built on this is testable without a particular
filesystem or a server.

Nothing overrides `askWhatIsOffered()` yet. The local filesystem's snapshots are
MOLE-200 and an object store's earlier versions are MOLE-201; each is a backend
answering one question, with the framework and its three states already here.
