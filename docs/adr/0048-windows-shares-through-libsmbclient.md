# ADR-0048: Windows shares through libsmbclient, and one session for the process

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

SMB is the protocol a Windows machine and almost every NAS offers first, and Mole
had no way to reach one. The alternative available today is mounting the share in
the operating system and browsing it as a local path.

## Decision

**Samba's own client library, `libsmbclient`, and not a mount.** Optional, through
`pkg_check_modules(SMBCLIENT …)` and a `MOLE_HAVE_SMB` definition, the way every
other backend dependency is: a build without it ships the other drives and says at
configure time which one it is missing.

**One session for the process, and every operation serialised behind it.**

## Reason

**Why not a mount.** The same reason ADR-0011 gives for dropping SSHFS rather
than writing it: a mount needs root, it does not port to Windows, and it puts a
drive in the operating system's namespace where every other application can see
it. A Mole drive is virtual and in-application.

**Why one session, when every other backend gets a resource per thread.** Because
the alternative was tried and it aborts inside Samba's allocator. A context per
thread was built first — the shape `SqliteTable` uses for its connections — and the
conformance suite ended in `talloc: access after free`, in `lib/param/loadparm.c`,
about eighty milliseconds in and nowhere near the call that caused it.

The cause is that **a context's function pointers are not the entry points.**
libsmbclient's plain `smbc_*` wrappers do bookkeeping around each call that
Samba's internals depend on — a talloc stackframe — and calling
`smbc_getFunctionStat(ctx)(…)` directly skips it. Samba then runs with no
stackframe, says so once (`no talloc stackframe at source3/lib/interface.c`), leaks
into its arena, and aborts later somewhere unrelated. The wrappers act on one
global context, set with `smbc_set_context()`, so using them means having one.

So the mutex protects two process-wide things rather than one: the context every
wrapper acts on, and the credentials the authentication callback reads back, which
belong to whichever drive is asking.

**The cost is real and worth stating.** A listing on one SMB drive waits for a read
on another. It is the price of using the library the way it is meant to be used,
and the alternative on offer was a backend that could take the process down.

## Consequences

- **A context is never freed.** `smbc_free_context()` tears down state
  libsmbclient keeps globally, so freeing it while the process still means to do
  SMB aborts in the allocator. One context for the life of the process is the
  bounded version of that.
- **`remove()` takes no session of its own**, because it calls `stat()`, `list()`
  and itself, each of which takes one, and the guard is not recursive.
- **Windows semantics show through, and two of them had to be written down.** A
  share refuses to unlink or replace a file somebody has open, where POSIX allows
  both — so the conformance suite now lets go of a read handle before removing or
  overwriting, which is what it always meant. And Samba's rename will replace an
  existing name, so the refusal every other backend gives is ours to make
  explicitly; a rename that silently overwrites is how a bulk rename destroys a
  file nobody mentioned.
- **Writes go under a working name and are renamed into place**, the same rule as
  the local disk (ADR-0020, ADR-0021). Without it an abandoned write leaves a
  partial file under the name somebody asked for.
- **Two library traps are commented where they were met**, because both cost an
  evening and neither is guessable: `smbc_dirent` ends in a flexible array
  declared `char name[1]`, which is enough for `QString::fromUtf8` to bind to its
  `QByteArrayView` overload and hand back a one-character name for every entry;
  and `smbc_setOptionProtocols()` takes a `char*` the library keeps an interest
  in, so a string literal corrupts the heap a few operations later.
- **NFS is not this.** It is MOLE-213, and libnfs will have traps of its own.
