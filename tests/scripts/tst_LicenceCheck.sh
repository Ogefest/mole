#!/usr/bin/env bash
#
# The guard on the Qt LGPL conditions, and whether it can fail for the reason it
# exists.
#
# It could not. `scripts/licence-check.sh` asked whether the paperwork was in the
# working directory, and `make bundle` ran it from the repository root -- so it was
# asking whether *this repository* carries LICENSE, which it always does. An
# artefact carrying none of the five files passed. Nothing published was wrong,
# because `make bundle` copies them itself; what was wrong is that the check was
# green because of where it ran. See MOLE-298.
#
# So the case that matters here is the one that could not exist before: take an
# artefact, delete its paperwork, and watch the check refuse it. Built out of a
# temp tree and the real binary, which the harness is told where to find -- the
# first two questions the script asks are about a binary, and a fake one would make
# this test about nothing.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

CHECK="$MOLE_SOURCE_DIR/scripts/licence-check.sh"

# An artefact of the shape a bundle or an AppDir has: the paperwork at the top.
bundle_at() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/usr/bin" "$root/licenses"
    cp "$MOLE_BINARY" "$root/usr/bin/mole"
    cp "$MOLE_SOURCE_DIR/LICENSE" "$MOLE_SOURCE_DIR/NOTICE" \
        "$MOLE_SOURCE_DIR/THIRD-PARTY-NOTICES.md" "$root/"
    cp "$MOLE_SOURCE_DIR"/licenses/*.txt "$root/licenses/"
}

# And of the shape a .deb or an .rpm unpacks to: the paperwork where the install
# rules put it.
installed_at() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/usr/bin" "$root/usr/share/doc/mole/licenses"
    cp "$MOLE_BINARY" "$root/usr/bin/mole"
    cp "$MOLE_SOURCE_DIR/LICENSE" "$MOLE_SOURCE_DIR/NOTICE" \
        "$MOLE_SOURCE_DIR/THIRD-PARTY-NOTICES.md" "$root/usr/share/doc/mole/"
    cp "$MOLE_SOURCE_DIR"/licenses/*.txt "$root/usr/share/doc/mole/licenses/"
}

# Run from somewhere that is neither the repository nor the artefact, because where
# it is run from is the whole subject.
check_from_elsewhere() {
    ( cd / && bash "$CHECK" "$@" ) > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

if [ -z "${MOLE_BINARY:-}" ] || [ ! -x "${MOLE_BINARY:-}" ]; then
    begin "there is a binary to build an artefact around"
    echo "  skipped: no application binary in this build (MOLE_BINARY=${MOLE_BINARY:-unset})"
    done_testing
fi

begin "an artefact carrying its paperwork is compliant, from any directory"
bundle_at "$SHELLTEST_TMP/whole"
check_from_elsewhere "$SHELLTEST_TMP/whole/usr/bin/mole" "$SHELLTEST_TMP/whole"
exited 0
said "Compliant."
# The source-tree question is answered too, and that is not free: run from
# elsewhere, a search of the working directory would have found no CMakeLists.txt
# and reported no GPL-only Qt modules -- a pass by absence.
said "no GPL-only Qt modules are used"

begin "the same artefact with its paperwork deleted is refused"
# The case that could not exist before this ticket.
rm -f "$SHELLTEST_TMP/whole/NOTICE" "$SHELLTEST_TMP/whole/licenses/LGPL-3.0.txt"
check_from_elsewhere "$SHELLTEST_TMP/whole/usr/bin/mole" "$SHELLTEST_TMP/whole"
[ "$SCRIPT_STATUS" != 0 ] || fail "an artefact with no NOTICE and no LGPL text passed"
said "missing: $SHELLTEST_TMP/whole/NOTICE"
said "missing: $SHELLTEST_TMP/whole/licenses/LGPL-3.0.txt"
said "Not compliant."

begin "an artefact with no paperwork at all is refused rather than skipped"
rm -rf "$SHELLTEST_TMP/bare"
mkdir -p "$SHELLTEST_TMP/bare/usr/bin"
cp "$MOLE_BINARY" "$SHELLTEST_TMP/bare/usr/bin/mole"
check_from_elsewhere "$SHELLTEST_TMP/bare/usr/bin/mole" "$SHELLTEST_TMP/bare"
[ "$SCRIPT_STATUS" != 0 ] || fail "an artefact with no licence paperwork passed"
said "no licence paperwork under"

begin "the installed layout is understood, which is what a .deb unpacks to"
installed_at "$SHELLTEST_TMP/installed"
check_from_elsewhere "$SHELLTEST_TMP/installed/usr/bin/mole" "$SHELLTEST_TMP/installed"
exited 0
said "usr/share/doc/mole/LICENSE"
# No bundled Qt in a distribution package, which is a different answer rather than
# a failure -- and it is said rather than passed over.
said "nothing to keep replaceable"

begin "the artefact root is derived from the binary when it is not given"
bundle_at "$SHELLTEST_TMP/derived"
check_from_elsewhere "$SHELLTEST_TMP/derived/usr/bin/mole"
exited 0
said "artefact root: $SHELLTEST_TMP/derived"

begin "with no artefacts to look at, it says so rather than passing"
# A sweep that found nothing is not a compliant release. Run in a copy of the
# repository with no dist/ and no packages, so the sweep has nothing to answer for.
rm -rf "$SHELLTEST_TMP/empty"
mkdir -p "$SHELLTEST_TMP/empty/scripts"
cp "$CHECK" "$SHELLTEST_TMP/empty/scripts/"
cp "$MOLE_SOURCE_DIR/CMakeLists.txt" "$SHELLTEST_TMP/empty/"
( cd / && bash "$SHELLTEST_TMP/empty/scripts/licence-check.sh" ) > "$SCRIPT_OUTPUT" 2>&1
SCRIPT_STATUS=$?
[ "$SCRIPT_STATUS" != 0 ] || fail "a sweep with no artefacts reported compliant"
said "no artefacts found"

done_testing
