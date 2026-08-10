# ADR-0028: A console runner, next to the window rather than inside it

- **Date:** 2026-08-10
- **Status:** Accepted

## Context

Every fault worth fixing in this project so far was found by copying a real file
to a real server and watching what happened: a read that stopped just short of a
gibibyte, an upload acknowledged and lost, a stall that took two minutes to
become an error. Reproducing one of those meant starting the window, configuring
a drive, opening two panes and dragging something — by hand, every time, on a
machine that had to have a display.

The test suite covers the other half. It cannot cover this half: a scenario in
C++ runs against a fake, and the whole point of these runs is that the server is
real and is the thing being doubted. Nor can a suite be put under `tc netem`, run
in a loop overnight, or handed to somebody with a shell and no desktop.

What phases 4 and 5 of the testing work need — a 100 GB transfer with peak
scratch space measured, a connection killed mid-copy, a disk filled on purpose —
is one command that starts one task and says what happened.

## Decision

**`mole-tasks`, a second binary in the same build**, linking `mole_core` and
`mole_host` and nothing above them. No `mole_ui`, no `mole_builtin`, no Qt Quick:
a `QCoreApplication`, the same task classes the window submits, and the same
plugins loaded from the same directory.

**It reaches drives exactly as the application does.** The same factories, the
same `drives.json`, the same credential store. A runner that connected its own
way would reproduce its own faults.

**Nine commands, one per task worth parameterising:** copy, move, delete, sync,
compress, rename, scan, duplicates, verify — plus `drives`, which says what is
mounted and how to address it.

**Exit codes mean something specific**: 0 done, 1 the work ran and something in
it failed, 2 the command line is wrong, 3 a drive could not be reached or
configured, 130 interrupted. A script in a loop has to tell those apart.

**`sync` and `rename` say what they would do and stop.** `--apply` carries it
out. Anything that can delete files does not do it on the strength of a typo.

**Secrets never appear in an argument.** A drive from the store is named with
`--drive`, and its passphrase comes from `MOLE_PASSPHRASE`. A drive described on
the command line takes `password=@SOME_VARIABLE`, which is read from the
environment. An argument list is readable by every process on the machine and a
shell history outlives the run.

**The work is a library and the executable is four lines of it**, so the commands
are driven by the test suite in-process — with one test that starts the binary
for real, because "runs with no display" is a claim about a process.

## Reason

**Why a second binary rather than a `--headless` flag on the first.** `mole`
links Qt Quick and creates a `QGuiApplication`; on a machine with no display that
is a connection that cannot be made, and a flag would not change it. Splitting
them also makes the layering visible: everything `mole-tasks` can do is
everything that lives below the interface, which is where it should be anyway.

**Why not a scenario language.** It was the obvious next thing to build and it is
the wrong thing. Scenarios are C++ tests, where a fault becomes a permanent
regression check. A YAML dialect for describing transfers would be a second,
worse language for the same job, with no compiler and no debugger.

**Why nine commands rather than a generic `run <task> --json '{…}'`.** The
generic form is shorter to write and unusable to type. This tool exists to be
typed, at two in the morning, by somebody trying to reproduce a fault.

**Why `--mount` at all, when there is a configuration file.** The file belongs to
whoever is sitting at the machine. A test rig, a CI job and a script driving
`tc netem` have no user and no desktop session, and asking them to write a
configuration file first — with credentials in it — is worse than a flag whose
secrets come from the environment.

## Consequences

- A fault found by hand is one command, and the same command goes into a shell
  script. Phase 4 (scale and interference) can be driven from a terminal.
- Two binaries are installed rather than one, and the package carries both.
- Everything the runner can do is bounded by what lives below `mole_ui`. A task
  that ends up implemented in the interface layer would not be reachable from
  here — which is a reason to keep putting them below it.
- The window and the runner share the drives file, the credential store and the
  index. Running a scan from the console updates the same catalogue the window
  searches, which is the point, and means a long scan started in a terminal is
  visible in the application afterwards.
