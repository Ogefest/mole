# Somewhere for a shell script to be tested.
#
# Most of what builds and damages the test environment is shell -- provisioning,
# the service configuration, the control channel -- and until MOLE-233 none of it
# was covered by anything. `tests/` is C++ through `mole_add_test`, there was no
# path that ran a shell script at all, and the cost of that was a guard in
# `services.sh` that had never once held: it exported the NFS share read-write to
# every host on the network for nine days and nobody could have caught it, because
# nothing ran the script.
#
# The shape is the cheapest one that works. These scripts do their damage through
# `ssh`, so a stub `ssh` earlier on `PATH` than the real one turns "provision a
# machine" into "write down what would have been sent to a machine" -- and the
# assertions are then made against a transcript, on a developer's laptop, with no
# testbed and no network.
#
# Sourced, not run:
#
#   . "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"
#
#   begin "the export is withdrawn when no client is named"
#   stub_ssh
#   run_script scripts/testbed/services.sh
#   transcript_lacks '/srv/moledata/nfs ('
#
#   done_testing
#
set -uo pipefail

MOLE_SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SHELLTEST_NAME="$(basename "${0%.sh}")"

# One tree per run, taken away afterwards however the run ends -- including the
# stub `ssh`, which must never outlive the test that put it on PATH.
SHELLTEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/$SHELLTEST_NAME.XXXXXX")"
trap 'rm -rf "$SHELLTEST_TMP"' EXIT

SHELLTEST_CASES=0
SHELLTEST_FAILED=0
SHELLTEST_CASE=""
SHELLTEST_CASE_FAILED=0
SHELLTEST_DUMPED=0

# What the script under test was told, and what it said back. Set by run_script.
TRANSCRIPT="$SHELLTEST_TMP/transcript"
SCRIPT_OUTPUT="$SHELLTEST_TMP/output"
SCRIPT_STATUS=0

# --- the cases ---------------------------------------------------------------

# Starts one. Everything asserted until the next `case` belongs to this one.
begin() {
    SHELLTEST_CASE="$1"
    SHELLTEST_CASE_FAILED=0
    SHELLTEST_DUMPED=0
    SHELLTEST_CASES=$((SHELLTEST_CASES + 1))
    : > "$TRANSCRIPT"
    : > "$SCRIPT_OUTPUT"
}

fail() {
    if [ "$SHELLTEST_CASE_FAILED" -eq 0 ]; then
        SHELLTEST_CASE_FAILED=1
        SHELLTEST_FAILED=$((SHELLTEST_FAILED + 1))
        printf 'FAIL: %s\n' "$SHELLTEST_CASE"
    fi
    printf '  %s\n' "$*"
}

pass_note() { [ -n "${VERBOSE:-}" ] && printf '  ok: %s\n' "$*"; return 0; }

done_testing() {
    printf '\n%s: %d cases, %d failed\n' "$SHELLTEST_NAME" "$SHELLTEST_CASES" "$SHELLTEST_FAILED"
    [ "$SHELLTEST_FAILED" -eq 0 ] || exit 1
    exit 0
}

# --- the stub ----------------------------------------------------------------

# An `ssh` that goes nowhere and writes down what it was asked to do.
#
# Both halves matter. The arguments say which machine and which account the
# script decided on; standard input is the script it would have run there, which
# is where every configuration file in `services.sh` lives. A test that saw only
# the arguments could not have caught the NFS fault, because the export line is
# in the heredoc.
#
# It exits 0, so the script under test proceeds as though the machine had
# answered. A stub that failed would only ever test the `die` branches.
stub_ssh() {
    mkdir -p "$SHELLTEST_TMP/bin"
    cat > "$SHELLTEST_TMP/bin/ssh" <<'STUB'
#!/usr/bin/env bash
{
    printf '=== ssh'
    for arg in "$@"; do printf ' %s' "$arg"; done
    printf '\n'
    cat
} >> "$SHELLTEST_TRANSCRIPT"
exit 0
STUB
    chmod +x "$SHELLTEST_TMP/bin/ssh"
    export SHELLTEST_TRANSCRIPT="$TRANSCRIPT"
    PATH="$SHELLTEST_TMP/bin:$PATH"
    export PATH
}

# --- running it --------------------------------------------------------------

# Runs a script from the repository, with its output kept rather than printed.
# The status is in SCRIPT_STATUS; the output is asserted on with `said`.
run_script() {
    local script="$1"; shift
    ( cd "$MOLE_SOURCE_DIR" && bash "$script" "$@" ) >"$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

# --- assertions --------------------------------------------------------------

# `reached` and `never_reached` are about the machine: they look at what the stub
# was sent. `said` is about the operator: it looks at what the script printed.
# Keeping them apart is deliberate -- announcing a skip and actually skipping are
# different claims, and the NFS fault was a script that made the first one while
# failing the second.

reached() {
    if grep -qF -- "$1" "$TRANSCRIPT"; then pass_note "reached the machine: $1"
    else fail "nothing sent to the machine contained: $1"; fi
}

never_reached() {
    if grep -qF -- "$1" "$TRANSCRIPT"; then
        fail "this reached the machine and should not have: $1"
        grep -nF -- "$1" "$TRANSCRIPT" | sed 's/^/    /' >&2
    else pass_note "never reached the machine: $1"; fi
}

never_reached_matching() {
    if grep -qE -- "$1" "$TRANSCRIPT"; then
        fail "a line matching /$1/ reached the machine"
        grep -nE -- "$1" "$TRANSCRIPT" | sed 's/^/    /' >&2
    else pass_note "nothing matches /$1/"; fi
}

reached_matching() {
    if grep -qE -- "$1" "$TRANSCRIPT"; then pass_note "matched /$1/"
    else fail "nothing sent to the machine matched /$1/"; fi
}

said() {
    if grep -qF -- "$1" "$SCRIPT_OUTPUT"; then pass_note "said: $1"
    else
        fail "the script never said: $1"
        # Once per case. Three missing lines in one case used to print the whole
        # run three times, which buries the assertions that explain it.
        if [ "$SHELLTEST_DUMPED" -eq 0 ]; then
            SHELLTEST_DUMPED=1
            printf '  what it said instead:\n'
            sed 's/^/    /' "$SCRIPT_OUTPUT"
        fi
    fi
}

exited() {
    [ "$SCRIPT_STATUS" = "$1" ] || fail "exited $SCRIPT_STATUS, expected $1"
}
