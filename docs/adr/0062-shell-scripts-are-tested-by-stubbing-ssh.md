# ADR-0062: Shell scripts are tested by stubbing ssh

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

The scripts that build and damage the test environment — `provision.sh`,
`services.sh`, `control.sh` and the tiers that drive them — were the least
covered code in the repository, and the only code with no way to cover it.
`tests/` was C++ throughout: `mole_add_test` compiles sources, links
`mole_test_support` and registers the binary with CTest. There was no path that
ran a shell script at all, and `shellcheck` was in neither `make tidy` nor
anywhere else.

The cost arrived as MOLE-233. The guard deciding whether to export the NFS share
was written `[ -z "\$NFS_CLIENTS" ]`, with a backslash belonging to a heredoc four
lines further down. The line runs in the outer script, so the test compared the
literal string `$NFS_CLIENTS` — never empty. The guard never held, the `else`
branch ran on every provisioning run with an empty client list, and `exportfs`
reads a missing host as `*`. Every machine the script had ever provisioned had
its NFS share exported read-write to every host on its network, for nine days,
under a comment saying the export must never be "the whole subnet by accident".
The skip branch the author wrote as the safe default was dead code that had never
printed once.

Nothing could have caught it. That is the decision this record is about.

## Decision

Shell scripts are tested in `tests/scripts/`, registered by a second hook —
`mole_add_shell_test` — that runs the file under `bash` instead of compiling it,
labelled `shell` so it runs in `make test` beside everything else.

They reach a machine through a **stub `ssh` placed earlier on `PATH`**, which
records its arguments and its standard input to a transcript and exits 0. The
test then asserts on the transcript. `tests/support/shelltest.sh` holds the stub,
the temp tree and the assertions, and separates two questions that the NFS fault
conflated: `reached`/`never_reached` ask what was sent to the machine, and `said`
asks what the operator was told.

Two kinds of case, and both are wanted:

- **Behavioural** — `tst_TestbedProvisioning.sh` runs `services.sh` with and
  without `MOLE_TESTBED_NFS_CLIENTS` and asserts on what would have been sent.
- **Static** — `tst_ShellScripts.sh` holds the rules that can be checked by
  reading, over every script at once, so a new script joins the suite by
  existing rather than by somebody remembering to add it.

## Reason

**A stub `ssh` rather than a container or a virtual machine.** Everything these
scripts do to a machine goes through one function — `on_server`, a single `ssh`
reading a heredoc on stdin — so intercepting `ssh` intercepts all of it. The
alternative was a throwaway container per test, which would have been slower than
the whole rest of the suite, would need Docker on every developer's machine, and
would test that Debian can install `nfs-kernel-server` rather than testing our
decisions. What we needed to assert is *what the script decided to send*, and the
transcript is exactly that. It runs in 0.1 s with no network.

**A second CMake hook rather than a C++ test that shells out.** A C++ wrapper
would have compiled, linked Qt and started a process to run `bash`, so a failure
would be reported as a C++ assertion about an exit code with the interesting part
in captured output. `add_test(COMMAND bash …)` is what CTest is for.

**Static rules in a test rather than `shellcheck` in `make tidy`.** `shellcheck`
flags this class directly and is the better long-term answer, but it is not
installed here and is not a build dependency, so adding a target for it would
have meant committing a gate nobody on this machine could run. The rules that
matter are held instead: every script parses, every script sets `-u`, no line
that runs locally defers expansion to a machine, and no private address is
written into a tracked file. `shellcheck` can join later and take over the first
three; the last is a rule of this repository that no linter knows.

**The checker announces when it loses count.** The deferred-expansion rule needs
to tell an outer line from a heredoc body, which means tracking heredoc depth by
reading. That is imperfect — while it was being written, `<<<"hello"` in
`check-services.sh` read as `<` followed by `<<"hello"`, opening a heredoc that
never closed and silently skipping every file after it. The check went green by
looking at nothing, which is worse than not having it. So depth resets per file,
and a heredoc it cannot close is reported as a finding rather than assumed away.

## Consequences

- Provisioning logic can be changed with a test to catch it, on a laptop, with no
  testbed and no network. The NFS guard specifically: reverting it turns
  `tst_TestbedProvisioning` red with the offending export line printed.
- Every script under `scripts/` is now inside a static check by existing. A new
  one that does not parse, does not set `-u`, or names a real address fails the
  suite without anybody adding it to a list.
- The stub tests what was *sent*, not what a server *does* with it. An export
  line that is correctly formed and wrong about how `exportfs` behaves would pass.
  That question belongs to `check-services.sh` against a real machine, and the two
  answer different things — the same split as the live suites.
- `bash` becomes a test-time dependency. It already was, for `make`.
- If `shellcheck` is ever added as a dependency, the first three static cases
  become redundant and should be dropped rather than kept alongside it.
