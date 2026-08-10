# ADR-0025: A role reads its own credentials from a file, and the role is the directory

- **Date:** 2026-08-10
- **Status:** Accepted
- **Follows:** [ADR-0024](0024-planning-and-engineering-write-as-themselves.md),
  which decided that planning and engineering write to the board as separate
  accounts. This is how a session ends up holding the right one.

## Context

Two roles, two sets of credentials, and a session has to arrive holding the right
one without being told. ADR-0024 said the token comes from `VIKUNJA_TOKEN` in the
session's own environment and left the mechanism open; there turned out to be
fewer workable mechanisms than expected.

**A shell profile cannot do it.** Both roles run as the same user on the same
machine, so `~/.profile` can only set one value for one variable — and the shell
a session runs commands in is non-interactive, which makes `~/.bashrc` the wrong
hook as well: the standard one returns before its first line of content.

**The harness has a per-directory settings file that could do it exactly right**,
and for one of the two directories that file is *inside this repository*. This
project's own rule is that a credential does not live in an ignored file inside a
checkout, because an ignored file is one `git add -f`, or one careless edit to
`.gitignore`, away from being published.

**Reading a value out of a document and pasting it into a command** is what
happens when nothing is set up, and it is the worst of the options: the credential
is then written into the session transcript, which is the one place nobody thinks
to look for it.

## Decision

**One file per role, outside every repository, and the role is the directory.**

Each role has a file of its own holding everything that role needs — the address,
its token, the account it acts as, and the name of the role. A session sources the
file its directory's `CLAUDE.md` names, in one line, and then works from ordinary
environment variables:

```sh
set -a; . <the role's file>; set +a
```

The files live beside the other facts a public repository cannot carry, at `600`
inside a `700` directory. A `CLAUDE.md` names the role of the directory it sits in
and the file that belongs to it; it never carries a value.

**Adding a role is adding a file.** Make the account, share the board with it at
the permission that role should have, drop the file next to the others, and name
it in the `CLAUDE.md` of the directory that role works in. No existing role is
touched, and nothing has to be reconciled.

## Reason

**The directory is already the thing that distinguishes the roles** — one works in
this checkout, one works where the planning is done — so deriving the role from it
adds no state that can disagree with reality. Anything else is a flag somebody has
to remember to set, and a session that forgets it acts as the wrong account
silently, which is precisely the failure ADR-0024 exists to prevent.

**A file per role rather than a file per value** because a role's file is then
self-contained: one `.` and the session is working. The address is repeated in
each, which is duplication of something that is not a secret, and worth it.

**Sourcing rather than reading the value at each use** because the recipes in
`CLAUDE.md`, and the migration tool, already speak in `$VIKUNJA_URL` and
`$VIKUNJA_TOKEN`. Sourcing makes ADR-0024's sentence true rather than aspirational,
and none of it had to change.

## Consequences

**A session works with nothing set up in advance**, which is the point: a fresh
session in this checkout reads one line of its own instructions and can talk to the
board. Nothing is inherited from a shell, so nothing breaks when a shell changes.

**The credential reaches `curl`'s argument list**, so another user on that machine
could read it from `ps`. There is one user, and an environment variable is readable
from `/proc` just the same — this is not a regression, but it is not zero either.

**A lost file is a lost token.** The files hold the only copies, and the board's
software shows an API token exactly once. Replacing one is a few minutes by hand;
the arrangement is worth knowing about before deleting anything.
