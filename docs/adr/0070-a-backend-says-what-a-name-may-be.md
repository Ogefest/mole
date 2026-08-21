# ADR-0070: A backend says what a name may be

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

Nothing between a name being chosen and a file being written asked whether the
destination would accept that name. On Linux almost everything is accepted, so
the gap never showed.

On Windows `< > : " | ? *`, a backslash, the control characters, a trailing dot
or space, and the device names `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`,
`LPT1`–`LPT9` are all refused by the filesystem — and the places that generate
names are exactly the places that meet them. `TransferTask` builds an arrival
name as `targetDirectory.child(source.fileName())`, and that same code names the
output of extracting an archive and of downloading from SFTP, S3, WebDAV or SMB,
where `really?.txt` and `a:b.txt` are perfectly legal. So a folder copied off a
NAS would fail partway through with an `IoError` carrying a path and no
explanation, having already written everything before the offending file.

`RenamePlan` rejected a new name for exactly one reason — it contains a
separator — so the bulk rename preview would show a plan of clean rows that the
backend was going to refuse one at a time. The tool exists precisely so somebody
can trust the preview before touching a hundred files.

## Decision

**The destination answers.** `IFileSystem::nameRules()` returns a `NameRules`
value describing what that drive will accept; the default is permissive, which
is what every protocol backend is and what this layer assumed silently before it
could be asked. `LocalFileSystem` answers with the platform's rules.

**`checkName(name, rules)` is a pure function** returning accept, or a reason and
a suggestion. It also refuses, before consulting any rule set, the names no
filesystem can hold: empty, `.`, `..`, anything containing a separator or a null.

**A suggestion is offered and never applied.** Sanitising silently is the wrong
default: a file that arrives under a name nobody chose is harder to find later
than one that did not arrive.

**The reason names the character.** "This name is invalid" tells somebody staring
at a hundred rows nothing they can act on.

Both callers follow: the rename preview marks the row before anything moves, and
a transfer fails that one file — top-level or deep inside a tree — rather than
stopping the run.

## Reason

**A rule set, not a platform.** Case folding went the same way in ADR-0068 and
for the same reason: the volume is what knows. A FAT-formatted stick on Linux is
stricter than the disk it is plugged into, and a bucket is stricter than neither.
A rule set is also data, so the Windows answers can be asserted on a Linux
machine — which is the only reason they are asserted at all.

**On `IFileSystem` rather than beside it**, because the caller that needs the
answer is always already holding the backend, and a free function taking a
platform would be one more thing to remember at each of the four call sites. It
is the same shape MOLE-194 is building for drive capabilities.

**The backslash is in the Windows set**, which is not obvious: it is not a bad
character there but a separator. A file called `back\slash.txt` copied to Windows
would not be a badly named file, it would be a file called `slash.txt` inside a
directory called `back`. That is a worse outcome than a refusal.

**Awkward is not the same question as refused.** A quote breaks a command line, a
hash breaks a url, a newline breaks a line-oriented protocol — and all three are
legal filenames on every platform Mole targets. Conflating the two would quietly
shrink what the awkward-names suite covers.

## Consequences

Four call sites can ask a question they could not ask before: the rename preview,
a transfer, the staging path a file is copied to before being opened
(MOLE-250), and the awkward-names suite, which can now split its table by asking
the rules rather than by trying and seeing (MOLE-252).

A backend that knows better than its platform can say so, and none does yet.
`LocalFileSystem` answers for the platform rather than for the volume, because Qt
has no portable way to ask a mounted volume — a FAT stick on Linux will still be
found out by the filesystem rather than by the preview.

`NameRules` describes what a name may *contain*. It says nothing about how long a
path may be, which is a different limit with a different failure and is
MOLE-248's.
