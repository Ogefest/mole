# ADR-0041: Git state is read through libgit2, and only read

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

A large share of the folders Mole is pointed at are checkouts, and until now it
had nothing to say about that: `git` appeared twice in the whole of `src/`, once
in a comment about a future drive and once as placeholder text in a sync field. A
file manager that shows which branch a folder is on, and which of its files have
changed, is telling somebody something they currently keep another window open
for.

Three ways to learn that were available, and they are not equally cheap.

**Spawning `git`.** One `git status --porcelain=v2 --branch` answers nearly all of
it in a single call, and the format is documented and stable.

**Parsing `.git` by hand.** `.git/HEAD` is one short read, and it gives the
branch.

**Linking libgit2.** A C library, packaged everywhere Mole is packaged, with the
whole of the plumbing behind it.

Alongside *how*, one more question had to be settled before any of it was built:
how far this goes. Showing state and changing it are not the same feature, and a
file manager drifting into being a git client is a thing that happens one
reasonable-looking commit at a time.

## Decision

**Git state is read through libgit2**, linked as an optional dependency in the
shape this repository already uses for Arrow, libvterm and OpenSSL:
`pkg_check_modules` at configure time, a `MOLE_HAVE_GIT2` compile definition, and
`Repository::isSupported()` answering no at run time when it is absent. A build
without `libgit2-dev` compiles, runs and behaves exactly as Mole did before this
existed — no band, no markers, nothing that fails.

`MOLE_WITH_GIT2=OFF` forces that build on a machine that does have the library,
because a configuration nobody can try is a configuration nobody does try.

**And it is only read.** Mole shows git state and does not change it: no staging,
no committing, no checking out, no fetching, no discarding. Every call in
`src/core/vcs/` is a read, and the first thing that writes to a repository is a
different project.

## Reason

**Against spawning `git`.** A process launch on every directory change, and a
directory change is the most common thing anybody does in a file manager. Its
output has to be parsed, which is a second implementation of a format; it depends
on whichever git is installed and on the user's own `git config`, so two machines
can answer differently about the same checkout; and a scan already running cannot
be cancelled, only killed, which matters because the answer for a large work tree
takes seconds and the user has usually moved on by then. Mole spawns a process in
exactly one place today — `src/core/terminal/Pty.cpp` — and there the process is
the point.

**Against parsing `.git`.** It answers the branch in one read and then falls off a
cliff. Per-file status needs the binary index across its format versions, blob
hashing to compare against it, the racy-timestamp rules, and `.gitignore` matching
with negation and precedence between nested files. That is a reimplementation of
git whose failure mode is not an error but a *silently wrong answer* — a file
shown as unchanged when it is not. For this feature a missing answer is
acceptable and a wrong one is not, which disqualifies the approach on its own.

**For libgit2.** It is the plumbing, in process, cancellable — `git_status_foreach_ext`
calls back per path and stops when the callback says so — and it has one API for
the branch, the state, the status walk and the ahead/behind counts, so the
expensive walk and the cheap facts come from the same handle.

**For read-only.** A file manager that shows git state is useful to everybody with
a checkout. One that half-implements a git client is a worse `git` and a worse
Mole at once: the operations people actually want are the ones with consequences
— staging hunks, resolving conflicts, rewriting history — and those need an
interface built for them, not a strip above a file listing. The boundary is
written down here so that the next person to look sees a decision rather than work
somebody forgot to do.

## Consequences

- **libgit2 wants a real filesystem path**, so this is local drives only. On any
  other drive there is no band and no marker, the way the sidebar draws no
  capacity bar for a bucket that cannot report one. A repository on a remote drive
  would be a git backend behind `IFileSystemFactory`, which is a different
  feature; pulling `.git` across SFTP to decorate a listing is not.
- **A `git_repository` must not be used from two threads at once.** Handles are
  cached per repository and shared, so `Repository` takes its own lock on every
  call. Two panes asking about one checkout serialise; opening it, which is the
  expensive part, happens once.
- **libgit2 is initialised once and shut down once**, reference-counted by the
  open handles themselves rather than at start-up and exit — shutdown has to
  happen after the last handle is freed, and which static holds the last one is
  not something this code gets to decide.
- The status walk stats the whole work tree, so it is a `Task` on a worker,
  marked background, and cancelled when the user navigates away.
- Tests build their fixture repositories through libgit2 in a temporary directory
  (`tests/support/GitFixture.h`) with libgit2's config search paths emptied. No
  installed `git`, no network, and nothing from whoever is running the suite.
- A diff or a log viewer stays possible later as an `IPreviewProvider`, and
  composes with this without changing any of it.
