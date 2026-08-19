# ADR-0050: NFS through libnfs, and a mount that is leased rather than owned

Date: 2026-08-19

Status: accepted

## Context

NFS is what a Linux or BSD file server offers first, and what a NAS offers beside
SMB. Mole had no way to reach an export: the only route was mounting it in the
operating system and browsing it as a local path, which is the situation ADR-0048
had just fixed for shares.

## Decision

**libnfs, a userspace client, and not a mount.** Optional through
`pkg_check_modules(LIBNFS …)` and a `MOLE_HAVE_NFS` definition, exactly the way
`MOLE_HAVE_SMB` is: a build without it ships the other drives and says at
configure time which one it is missing.

**A context is leased, not owned.** An operation borrows a mounted connection for
its own duration and gives it back; an open file borrows one for as long as it is
open. Connections are pooled per server-and-export, and at most four are kept idle.

## Reason

**Why not a mount.** The reason ADR-0011 gives for dropping SSHFS and ADR-0048
gives for not mounting a share: it needs root on the machine running Mole, it does
not port, and it puts a drive in the operating system's namespace where every other
application can see it. A Mole drive is virtual and in-application.

**Why leased, when the ticket asked for one context per thread.** MOLE-213 named
the shape SMB had settled on and asked for the per-thread version of it — a context
built on first use, outliving every drive that used it, the way `SqliteTable` keeps
a connection per thread. That is right for the metadata calls and wrong for the
ones that matter: **a handle belongs to the context it was opened on.** `openRead`
hands back a `QIODevice`, and the thread that reads a file is not the thread that
opened it — a transfer opens on one pool thread and streams on another. A per-thread
context means the read is issued on a context that has never heard of the handle.

The other two candidates lose for stated reasons rather than by taste:

- **One context for the process**, as SMB has, would serialise every NFS operation
  behind one connection. SMB has no choice — its wrappers act on a global context
  — and libnfs has: a context is a self-contained object with a socket of its own
  and no global state behind it, so contexts really are independent. Paying SMB's
  cost without SMB's reason would be copying the scar and not the lesson.
- **A context per open** is correct and slow. Mounting is two round trips, so a
  directory walk would pay for a mount per directory. Pooling makes the second
  open free.

**Why four idle connections.** Enough that ordinary work — a listing, a copy, a
scan — never mounts twice, few enough that a drive holds a bounded number of
sockets rather than one per file that was ever read.

## Consequences

- **A broken connection is closed rather than returned.** libnfs keeps one TCP
  connection per context, and once it has gone every later call on that context
  answers with the same failure. So a failure is classified: the server answering a
  question about a file (`ENOENT`, `EEXIST`, `EACCES`, …) returns the connection to
  the pool, and anything else abandons it. Without that a single timeout leaves a
  drive broken until Mole is restarted.
- **Twenty seconds of patience per call**, set with `nfs_set_timeout()` — the same
  figure the HTTP transport gives a socket (ADR-0013's amendment) and for the same
  reason. Left to itself libnfs waits for the kernel, which on a silently dropped
  connection is minutes, and a window that has stopped answering is read as a hang
  rather than as an error.
- **NFS has no authentication and the form must not pretend otherwise.** The
  export list decides who may mount; after that the server believes whatever user
  id the client claims. So there is no password field, and the user id is an
  advanced field whose help says what it is — a claim, not a credential. Empty
  means the ids this process runs as, which is right whenever the accounts on both
  machines line up.
- **A directory opens as readily as a file, so `openRead` checks.** NFS has no
  open: libnfs looks the name up and hands back a handle, and a directory answers
  that lookup. Unchecked, reading a directory succeeds and then fails on the first
  read — which arrives as a failed copy rather than as a refused one. The
  `nfs_fstat64` that used to be skipped when the caller already knew the size is
  now always made, and it is what refuses.
- **Two library shapes are worth knowing before writing against it.** `nfs_read`
  and `nfs_write` take the count *before* the buffer, which compiles either way
  round with a `void*`; and a directory entry carries the attributes for free
  through READDIRPLUS, so unlike a share (ADR-0048) an ordinary listing needs no
  stat per entry — the fallback for a server offering only plain READDIR is there,
  and on the testbed it never runs.
- **A rename replaces, being POSIX**, so the refusal every backend owes the
  conformance suite is ours to make explicitly — and the commit of a finished write
  goes straight to `nfs_rename` without the unlink a share needs first.
- **Writes go under a working name and are renamed into place**, the same rule as
  everywhere else (ADR-0020, ADR-0021).
