#!/usr/bin/env bash
#
# The check that starts an artefact, and whether it can fail.
#
# It exists because every other artefact check could not: `--version` and
# `--plugins` answer without initialising a platform plugin, and the containers the
# artefacts are verified in have no X server to initialise one against -- so an
# AppImage that aborted on start on any machine without libxcb-cursor0 passed all of
# them. See MOLE-300.
#
# A guard written for that reason had better be able to fail, which is what this
# holds: two launchers that are wrong in the two ways that matter, and one that is
# right. Fake launchers rather than real artefacts, because building one takes
# minutes and what is under test here is the check.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

CHECK="$MOLE_SOURCE_DIR/scripts/check-artefact-starts.sh"

launcher() { # launcher <name> <body>
    local path="$SHELLTEST_TMP/$1"
    printf '#!/usr/bin/env bash\n%s\n' "$2" > "$path"
    chmod +x "$path"
    printf '%s' "$path"
}

run_check() {
    MOLE_START_DEADLINE=5 bash "$CHECK" "$@" > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

if ! command -v Xvfb >/dev/null 2>&1; then
    begin "there is an X server to start something against"
    echo "  skipped: no Xvfb on this machine, which is what the check needs"
    done_testing
fi

begin "a launcher that dies on start is refused"
# What a missing platform plugin does: Qt prints and aborts. This is the case that
# would have caught MOLE-300 and did not exist.
run_check "$(launcher dies 'echo "no Qt platform plugin could be initialized" >&2; exit 134')"
[ "$SCRIPT_STATUS" != 0 ] || fail "a launcher that aborted was accepted"
said "it exited with 134 before showing a window"
# What it said on the way down is repeated, because that is the line somebody needs.
said "no Qt platform plugin could be initialized"

begin "a launcher that stays up without a window is refused"
# The other half, and the reason the check waits for a window rather than for a
# process: something can start, keep running and draw nothing.
run_check "$(launcher mute 'sleep 60')"
[ "$SCRIPT_STATUS" != 0 ] || fail "a launcher that showed no window was accepted"
said "no window called"

begin "a launcher that opens a window is accepted"
# xdotool rather than a Qt application: what the check reads is the X server, so
# anything that maps a window with the right name is enough to hold that the passing
# path passes. Skipped where there is nothing to draw with, rather than assumed.
if command -v xterm >/dev/null 2>&1; then
    run_check "$(launcher shows 'xterm -T Mole -e sleep 60')" Mole
    [ "$SCRIPT_STATUS" = 0 ] || { fail "a launcher that opened a window was refused"; sed 's/^/    /' "$SCRIPT_OUTPUT"; }
elif command -v xmessage >/dev/null 2>&1; then
    run_check "$(launcher shows 'xmessage -name Mole hello')" Mole
    [ "$SCRIPT_STATUS" = 0 ] || { fail "a launcher that opened a window was refused"; sed 's/^/    /' "$SCRIPT_OUTPUT"; }
else
    echo "  skipped: nothing on this machine draws a window to check the passing path with"
    echo "  (make start-check does it for real, against the artefacts themselves)"
fi

begin "it says which display it used, so a failure can be looked at"
run_check "$(launcher dies 'exit 1')"
said "Start check:"
said "on :"

done_testing
