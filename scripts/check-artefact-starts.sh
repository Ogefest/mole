#!/usr/bin/env bash
#
# Does the artefact start, with a real platform plugin, and reach a window?
#
# **Every other artefact check asks a question a broken platform plugin cannot
# fail.** `--version` and `--plugins` answer without initialising one, and the
# containers the artefacts are verified in have no X server to initialise against --
# so the AppImage went out unable to start at all on any machine without
# libxcb-cursor0, which includes the workstation this project is developed on, and
# nothing noticed. `--plugins` had already replaced `--version` in those checks for
# exactly this class of fault, and was blind to this one. See MOLE-300.
#
# So this one starts it for real: an X server of its own, the platform plugin Qt
# would choose on a desktop, and an assertion that a window appeared. A launcher
# that aborts fails it within a second, which is what today's AppImage does.
#
# Usage:
#   scripts/check-artefact-starts.sh <launcher> [<name it should show>]
#
set -uo pipefail

LAUNCHER="${1:?usage: check-artefact-starts.sh <launcher> [window name]}"
WANTED="${2:-Mole}"
# Long enough for a cold start off a squashfs, short enough that a hang is a
# failure rather than a wait. A start that takes longer than this is a fault of its
# own.
DEADLINE="${MOLE_START_DEADLINE:-40}"

command -v Xvfb >/dev/null 2>&1 || {
    echo "skipped: no Xvfb, so there is no display to start against" >&2
    exit 3
}
[ -x "$LAUNCHER" ] || {
    echo "no launcher at $LAUNCHER" >&2
    exit 1
}

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mole-start.XXXXXX")
XVFB_PID=""
APP_PID=""
cleanup() {
    [ -z "$APP_PID" ] || kill "$APP_PID" 2>/dev/null
    [ -z "$XVFB_PID" ] || kill "$XVFB_PID" 2>/dev/null
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

# A display number nothing else is on. :99 by convention, and moved along if it is
# taken, because a machine that already runs one is the ordinary case.
DISPLAY_NUMBER=99
while [ -e "/tmp/.X${DISPLAY_NUMBER}-lock" ] && [ "$DISPLAY_NUMBER" -lt 120 ]; do
    DISPLAY_NUMBER=$((DISPLAY_NUMBER + 1))
done

Xvfb ":$DISPLAY_NUMBER" -screen 0 1280x900x24 -nolisten tcp > "$SCRATCH/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 50); do
    [ -e "/tmp/.X${DISPLAY_NUMBER}-lock" ] && break
    sleep 0.2
done
kill -0 "$XVFB_PID" 2>/dev/null || {
    echo "Xvfb did not start:" >&2
    cat "$SCRATCH/xvfb.log" >&2
    exit 1
}

echo "Start check: $LAUNCHER (on :$DISPLAY_NUMBER)"

# A profile of its own, so a session this leaves behind is not the author's, and no
# window the artefact restores comes from somebody's real session.
env DISPLAY=":$DISPLAY_NUMBER" \
    HOME="$SCRATCH/home" \
    XDG_RUNTIME_DIR="$SCRATCH/run" \
    "$LAUNCHER" > "$SCRATCH/app.log" 2>&1 &
APP_PID=$!
mkdir -p "$SCRATCH/home" "$SCRATCH/run"

# Two things can go wrong and they are told apart: it can die, or it can come up and
# show nothing. The first is what a missing platform plugin does.
found=0
for _ in $(seq 1 "$DEADLINE"); do
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        wait "$APP_PID"
        status=$?
        echo "  FAIL  it exited with $status before showing a window" >&2
        sed 's/^/        /' "$SCRATCH/app.log" >&2
        exit 1
    fi
    if command -v xwininfo >/dev/null 2>&1; then
        if grep -qF "$WANTED" <<<"$(xwininfo -display ":$DISPLAY_NUMBER" -root -children 2>/dev/null)"; then
            found=1
            break
        fi
    fi
    sleep 1
done

if command -v xwininfo >/dev/null 2>&1; then
    [ "$found" = 1 ] || {
        echo "  FAIL  it is running but no window called '$WANTED' appeared in ${DEADLINE}s" >&2
        sed 's/^/        /' "$SCRATCH/app.log" >&2
        exit 1
    }
    echo "  OK    it started and showed a window called '$WANTED'"
else
    # No xwininfo: still worth running, because surviving a start with a real
    # platform plugin is the half that fails today. Said rather than implied.
    echo "  OK    it started and stayed up (no xwininfo here, so the window was not looked for)"
fi

exit 0
