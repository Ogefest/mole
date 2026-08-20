#!/usr/bin/env bash
#
# Takes the guide's pictures twice over an unchanged tree and says whether a
# regeneration would be reviewable.
#
# `make guide-images` used to rewrite most of the guide whether or not anything had
# changed: fifty of fifty-three files, measured on 2026-08-20. A ticket that moved
# one thing on screen then produced a diff of fifty binary files, of which one was
# the change -- so nobody could tell whether the picture that mattered had moved,
# and a real regression in an unrelated picture was invisible. MOLE-255 is about
# making that diff mean something.
#
# What was moving, and what was done about it, in the order it mattered:
#
#   * **the drives' free space**, read from the real disk, in thirty-nine of the
#     fifty-three. `MOLE_DRIVES` now carries a size per drive, so the sidebar
#     reports an invented capacity instead of this machine's;
#   * **the clock**, in the date column of everything the run created rather than
#     the fixture. `fixDatesUnder()` gives every file a date derived from its own
#     path, so a file nobody remembered to list still gets a fixed one;
#   * **the order of a list**, in the sync plan and inside a duplicate group. Both
#     came out in whatever order the filesystem listed them; both now sort by path,
#     which a plan somebody reads before agreeing to it needed anyway;
#   * **the terminal's prompt**, which was the real shell of whoever ran the suite
#     -- so a *user name and machine name* were committed to a public repository.
#     The panel now starts a shell with no rc files and a prompt of our own;
#   * **the text caret**, which blinks, so the same state gave `report|` and
#     `report`. Qt is told not to flash it;
#   * **the task strip**, in every picture, counting jobs that had finished. A grab
#     now waits for the window to stop working first;
#   * **an animation's tail**: three identical frames rather than two, and a longer
#     budget, because a scrollbar fades itself out on a delay rather than in a
#     transition.
#
# **Byte-identical is not the bar, and measuring is why.** With all of the above
# fixed, what remains between two runs is one to five levels out of 255 in a few
# dozen pixels of pictures whose content is letter-for-letter the same: Qt's scene
# graph does not render a given frame to identical bytes twice. So the comparison
# is what an eye can see -- see compare-shots.cpp for the two numbers it takes.
#
# Usage:
#   scripts/check-screenshots.sh [build-dir]
#
set -uo pipefail

BUILD="${1:-build/debug}"
COMPARE="$BUILD/compare-shots"
WALKTHROUGH="$BUILD/tests/tst_Walkthrough"

[ -x "$WALKTHROUGH" ] || { echo "Build first: no $WALKTHROUGH" >&2; exit 2; }
[ -x "$COMPARE" ] || { echo "Build first: no $COMPARE" >&2; exit 2; }

# The pictures that cannot be identical twice running, each for a reason, and each
# reason a property of what the picture is *of* rather than of how it is taken. A
# name here is a claim that has to be defended, so keep it short and keep it true.
#
#   01d-slow-folder            a folder still loading, caught on purpose: the point
#                              of the picture is the state between empty and listed
#   02b-preview-csv-loading    a CSV part-read, with a busy indicator that never
#                              stops turning while it is the subject
#   24-transfer-running        a transfer in flight, with the bytes moved and the
#                              rate on screen
#   08-automation              a run's row says how long it took: "26 ms" one time
#                              and "16 ms" the next, and the duration is the content
#   16-alerts                  when the check ran, to the minute
#   17-reports                 how long the analysis took
#   26-indexes                 **seen once and never again.** It moved on 2026-08-20
#                              by 6652 pixels, and did not recur in eight further
#                              pairs of runs, two of them under a load average above
#                              seven. The one clock in that view is the row's
#                              "scanned just now", which `ageInWords()` turns into
#                              "2 minutes ago" at exactly 120 seconds -- but the step
#                              scans and photographs well inside that even under
#                              load, so it is a mechanism without evidence rather
#                              than a cause. Named so the check stays usable, and it
#                              comes out when MOLE-261 closes on a second sighting.
#                              **A sighting now explains itself**: the line names the
#                              box the differences fall inside and both copies are
#                              kept, which is what was missing the first time.
#   13-compress, 14-delete     the 📄 and 📁 glyphs in the list of what an operation
#                              is aimed at do not rasterise to the same pixels twice:
#                              two 7x10 patches, up to 37 levels out, in the icon
#                              column. Named with the real reason after being blamed
#                              on a scrollbar in MOLE-260 -- the 41-pixel span
#                              between the two icons was read as a scrollbar's height
#                              without anybody looking at the pixels. MOLE-266.
#
# Everything else must be identical. Anything new appearing below is a regression
# in determinism, and the honest response is to fix it rather than to add a line.
EXPECTED='01d-slow-folder,02b-preview-csv-loading,24-transfer-running,08-automation,16-alerts,17-reports,26-indexes,13-compress,14-delete'

# A pixel differs when a channel is more than eight levels out -- below that is the
# renderer's own noise, invisible and unavoidable. A picture has changed when even
# one such pixel is found: the tolerance is about how *different* a pixel is, never
# about how many, or a real one-word change would slip through.
TOLERANCE=8
PIXELS=0

# Where a sighting is left behind. Both runs go into temporary directories that are
# taken away on exit, and that is how MOLE-261 got away: `26-indexes` moved once, the
# run printed a pixel count, and by the time anybody asked what had moved both copies
# were gone. Anything that changes is copied out to here instead, so the next
# occurrence explains itself without somebody having to be watching.
KEPT="${MOLE_SHOTS_KEPT:-$BUILD/screenshots-changed}"
rm -rf "$KEPT"

first="$(mktemp -d)"; second="$(mktemp -d)"
trap 'rm -rf "$first" "$second"' EXIT

for pass in "$first" "$second"; do
    printf 'taking the pictures into %s\n' "$pass"
    if ! QT_QPA_PLATFORM=offscreen MOLE_SCREENSHOT_DIR="$pass" "$WALKTHROUGH" >"$pass/.log" 2>&1; then
        echo "the walkthrough failed; the pictures are not worth comparing" >&2
        tail -30 "$pass/.log" >&2
        exit 2
    fi
done

echo
"$COMPARE" "$first" "$second" --tolerance "$TOLERANCE" --pixels "$PIXELS" --allow "$EXPECTED" \
    --keep "$KEPT"
status=$?
echo
if [ "$status" -eq 0 ]; then
    echo "Regenerating the guide would rewrite only what changed."
else
    echo "A picture moved with nothing changed. Either find what is not fixed yet," >&2
    echo "or -- if it is genuinely a picture of something in motion -- name it in" >&2
    echo "EXPECTED above, with the reason." >&2
    echo >&2
    echo "Both versions of each one are in $KEPT, and the line above says which" >&2
    echo "box the differences fall inside. Look before re-running: it may not come back." >&2
fi
exit $status
