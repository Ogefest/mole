# ADR-0090: A credential store is read with the cost its own file names

- **Date:** 2026-09-03
- **Status:** Accepted

## Context

`SecretStore` gets the cryptography right — a fresh nonce per write, the whole
header authenticated so an attacker cannot weaken the derivation parameters and
have the file accept the result, the key zeroed on lock. It got four things
around the cryptography wrong, and the first of them is the one this record is
named after.

The scrypt cost sits in the file header, and the comment on the constants says
why: "stored in the file so a future build can raise them without orphaning
existing stores". `unlock()` read the n, r and p out of the header and derived
with them — and then kept only the key and the salt. Every later write built the
header from the compile-time constants. The two agree only while the constants
have never changed. The first `setSecret()` after a build that raised them would
write a header whose cost no longer matched the key, and the next `unlock()` would
report "Wrong passphrase, or the file has been altered" for ever. Every credential
lost, in a way indistinguishable from a forgotten passphrase.

Three more, in the same file. A store cut back to exactly its header opened
under *any* passphrase: `decrypt()` answers empty for anything shorter than a
tag, the failure test was `plaintext.isEmpty() && !sealed.isEmpty()`, and for a
header with nothing after it both halves are false — so the store was marked
unlocked and empty with no tag ever checked, and the next write re-keyed the file
under whatever had been typed. A write that failed left the change in memory, so
`secret()` answered with something the file had never held; worse,
`changePassphrase()` assigned the new salt and key *before* writing, so a failed
write left the file on the old passphrase, the object on the new one, and the
next successful write produced a file only the passphrase the user had been told
was rejected would open. And the 0600 was set after `commit()` and its answer
dropped, while `QSaveFile` widens its temporary to `0666 & ~umask` when the
target does not exist.

Outside the store: the derivation is a noticeable fraction of a second *by
design*, and the unlock dialog ran it in its button handler, on the thread that
draws, with nothing saying it was working.

## Decision

**A store is read with the cost its own header names, and that cost is what the
next write puts back.** `SecretStore::Cost` is remembered beside the key and the
salt: read from the file on unlock, chosen by this build only where a key is
*made* — `create()` and `changePassphrase()`. Raising the constants therefore
upgrades a store the next time its passphrase is changed, and orphans nothing in
the meantime, which is what the numbers being in the header was always for.

**`create()` takes the cost as a parameter.** The default is this build's choice
and every caller uses it; the parameter exists because the promise above cannot
be tested without writing a store at a different one, and a promise nothing
checks is the promise that was broken here.

**Anything shorter than a tag is "the file has been altered".** A store this code
writes is never header-only: even an empty one seals to sixteen bytes.

**A mutator writes first and believes the result second.** Each keeps what it
would have to put back — the map, the key, the salt and the cost — and restores
it when the write does not land, so the object and the file never disagree.

**The mode is set on the `QSaveFile` before `commit()`, and a refusal fails the
write.** The file is then never in place with any other mode, and a promise the
class makes on its own page is not quietly not kept.

**Every public method takes the store's own lock**, and the shell derives on a
task with the button disabled and a busy indicator beside the field.

## Reason

**Why a recursive mutex.** `changePassphrase()` is `unlock()` followed by a
write. Splitting it to avoid the second acquisition would mean two ways of
opening a store, which is one more than a class like this should have.

**Why lock the whole store rather than take the derivation off it.** The
alternative was to split unlocking into "read the header", "derive" and "install"
so the expensive part touches nothing — attractive, and it exposes the file's
internals through the public interface to do it. The store is small, its
operations are rare and already slow, and it is reached from `RemoteRegistry` and
from `mole-tasks` as well as from the window; one lock makes all of those safe
rather than the one the dialog happens to use.

**Why `QtConcurrent` and not a `Task`.** A `Task` appears in the task list with a
title, a progress bar and a line in the session log. "Unlock credentials" is a
modal dialog's own business for half a second, and a row in the strip for it
would be noise — and the strip is behind the modal in any case.

**Why the parameters were not simply pinned to the constants.** That is the other
way to make the two agree, and it throws away the upgrade path the header was
built for: a build that raises the cost would then be unable to read anything
written before it.

## Consequences

A store written by any build opens under any other, at whatever cost its own
header names, and a raised constant reaches it the next time its passphrase
changes. A damaged or truncated file is refused rather than opened empty. A write
that could not land leaves the object exactly as the file has it, including the
passphrase.

The store is now safe to use from any thread, which is what lets the window stay
live while a passphrase is turned into a key — and what makes the next thing that
wants to read a credential off the drawing thread simply able to.

`tests/core/tst_SecretStore.cpp` holds all four: a store written at another cost
surviving its own next write, a file cut to its header being refused under every
passphrase, a write into a directory nobody may write into leaving `secret()` and
the file agreeing, and a failed change of passphrase leaving the old one the only
one that opens the store.

The lock has two cases of its own there, and neither can fail on its own: a
store opening on one thread while another asks it what the window asks reads the
same guarded or not, and it is `make tsan TESTS=SecretStore` that tells the two
apart. They are two because each asks one question — taking a mutex orders
everything the taker did before it, so a loop asking all of them would have its
guarded calls order its unguarded read against the worker's write, and with the
lock taken back out of `isUnlocked()` the sanitizer duly found nothing. That is
also why `isUnlocked()` takes the lock, cheap as a bool looks: `unlock()` sets it
from whichever thread derived the key.
