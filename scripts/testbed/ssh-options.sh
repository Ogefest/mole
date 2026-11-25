# shellcheck shell=bash
#
# Which key the testbed scripts sign in with. Sourced, not run.
#
# Without this, every ssh these scripts make walks the agent and the default
# files in ssh's own order. On a workstation whose keys are hardware-backed,
# that order starts with keys that need a physical touch to sign. BatchMode
# turns each one into a failure, and a server that counts attempts may close
# the connection before reaching a key that could have signed. MOLE-354 stopped
# on exactly that. See MOLE-468.
#
# MOLE_TESTBED_SSH_KEY names a private key, a path on this machine and outside
# the repository. When it is set, every ssh uses that key and only that key. When
# it is unset, TESTBED_SSH is empty and nothing changes, so a workstation where
# the default key works keeps working.
#
#   ssh "${TESTBED_SSH[@]}" -o BatchMode=yes "$NODE" …
#
# TESTBED_SSH_WORDS is the same list as one string, for MOLE_TEST_CONTROL. The
# suites split that variable on spaces (tests/support/TestbedControl.cpp), which
# is why a key path with whitespace in it is refused rather than quoted.

# Every script that sources this already runs with set -u. Said here too, so
# this file is held to the same rule when it is read on its own.
set -u

TESTBED_SSH=()
TESTBED_SSH_WORDS=""
if [ -n "${MOLE_TESTBED_SSH_KEY:-}" ]; then
    case "$MOLE_TESTBED_SSH_KEY" in
        *[[:space:]]*)
            echo "MOLE_TESTBED_SSH_KEY has whitespace in it; MOLE_TEST_CONTROL is split on spaces, so use a path without any." >&2
            exit 2
            ;;
    esac
    if [ ! -r "$MOLE_TESTBED_SSH_KEY" ]; then
        echo "MOLE_TESTBED_SSH_KEY names $MOLE_TESTBED_SSH_KEY, which cannot be read." >&2
        exit 2
    fi
    TESTBED_SSH=(-i "$MOLE_TESTBED_SSH_KEY" -o IdentitiesOnly=yes)
    TESTBED_SSH_WORDS="-i $MOLE_TESTBED_SSH_KEY -o IdentitiesOnly=yes"
fi
