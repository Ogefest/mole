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
# and reported nothing at all -- a pass by absence.
said "every Qt module the build asks for is on the LGPL allowlist"

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

# ---- what the notices have to name, and what has to travel ------------------

begin "every library the build looks for is named in the notices"
# Six were not: libsmbclient, libgit2, libnfs, libvterm, Arrow and Parquet. A
# dependency with no row ships unnamed, and the notices said the network
# plugin's only dependency was curl.
bundle_at "$SHELLTEST_TMP/named"
check_from_elsewhere "$SHELLTEST_TMP/named/usr/bin/mole" "$SHELLTEST_TMP/named"
exited 0
said "every library the build looks for has a row in THIRD-PARTY-NOTICES.md"

begin "a library with no row in the notices fails the check"
# Asked of a copy of the source tree, because the question is about *this*
# repository's CMake files and the answer has to be able to be no.
rm -rf "$SHELLTEST_TMP/tree"
mkdir -p "$SHELLTEST_TMP/tree/scripts" "$SHELLTEST_TMP/tree/src"
cp "$CHECK" "$SHELLTEST_TMP/tree/scripts/"
cp "$MOLE_SOURCE_DIR/CMakeLists.txt" "$SHELLTEST_TMP/tree/"
cp "$MOLE_SOURCE_DIR/THIRD-PARTY-NOTICES.md" "$SHELLTEST_TMP/tree/"
cat > "$SHELLTEST_TMP/tree/src/CMakeLists.txt" <<'CMAKE'
pkg_check_modules(FLUXCAP QUIET fluxcapacitor)
CMAKE
bundle_at "$SHELLTEST_TMP/unnamed"
( cd / && bash "$SHELLTEST_TMP/tree/scripts/licence-check.sh" \
    "$SHELLTEST_TMP/unnamed/usr/bin/mole" "$SHELLTEST_TMP/unnamed" ) > "$SCRIPT_OUTPUT" 2>&1
SCRIPT_STATUS=$?
[ "$SCRIPT_STATUS" != 0 ] || fail "a library with no row in the notices passed"
said "linked but not named in THIRD-PARTY-NOTICES.md: fluxcapacitor"

begin "a Qt module that is not on the LGPL allowlist fails the check"
# The rule used to be a fixed list of five GPL-or-commercial names, so Qt Quick
# 3D, Qt Graphs and everything released since passed unexamined.
cat > "$SHELLTEST_TMP/tree/src/CMakeLists.txt" <<'CMAKE'
find_package(Qt6 REQUIRED COMPONENTS Core Charts)
CMAKE
( cd / && bash "$SHELLTEST_TMP/tree/scripts/licence-check.sh" \
    "$SHELLTEST_TMP/unnamed/usr/bin/mole" "$SHELLTEST_TMP/unnamed" ) > "$SCRIPT_OUTPUT" 2>&1
SCRIPT_STATUS=$?
[ "$SCRIPT_STATUS" != 0 ] || fail "a Qt module nobody has checked the licence of passed"
said "Qt module not on the LGPL allowlist: Charts"

begin "the licence texts required are the ones the notices name"
# A fixed list of five files could not fail for the four texts that were never
# added, which is why LGPL-2.1, GPL-2 with the exception, MIT and BSD were
# missing from licenses/ while the notices said the LGPL components need theirs.
bundle_at "$SHELLTEST_TMP/derived-texts"
rm -f "$SHELLTEST_TMP/derived-texts/licenses/LGPL-2.1.txt"
check_from_elsewhere "$SHELLTEST_TMP/derived-texts/usr/bin/mole" "$SHELLTEST_TMP/derived-texts"
[ "$SCRIPT_STATUS" != 0 ] || fail "an artefact missing a text its own notices name passed"
said "missing: $SHELLTEST_TMP/derived-texts/licenses/LGPL-2.1.txt"

begin "notices naming no licence text at all is a failure rather than a pass"
bundle_at "$SHELLTEST_TMP/silent"
printf '# Third-party notices\n\nNothing to declare.\n' \
    > "$SHELLTEST_TMP/silent/THIRD-PARTY-NOTICES.md"
check_from_elsewhere "$SHELLTEST_TMP/silent/usr/bin/mole" "$SHELLTEST_TMP/silent"
[ "$SCRIPT_STATUS" != 0 ] || fail "notices requiring nothing passed"
said "names no licence text at all"

# ---- the binary, as it is actually published --------------------------------

begin "a stripped binary is still asked whether Qt is inside it"
# Every published binary is stripped -- by the Makefile, by CPACK_STRIP_FILES and
# by package-appimage.sh -- and `nm --defined-only` reads the table strip
# removes. So the check reported "none" by absence on exactly the artefacts it
# exists for, and this suite could not see it because it runs the build binary.
if ! command -v strip >/dev/null 2>&1; then
    echo "  skipped: no strip on this machine"
else
    bundle_at "$SHELLTEST_TMP/stripped"
    strip "$SHELLTEST_TMP/stripped/usr/bin/mole"
    check_from_elsewhere "$SHELLTEST_TMP/stripped/usr/bin/mole" "$SHELLTEST_TMP/stripped"
    exited 0
    said "no Qt symbols compiled into the binary"
fi

begin "a binary that really does define a Qt symbol is refused"
# The other half: a check that never fails is indistinguishable from one that
# passes. This is the smallest thing that looks like a statically linked Qt --
# an executable exporting QCoreApplication::exec() from its own dynamic table.
if ! command -v g++ >/dev/null 2>&1; then
    echo "  skipped: no g++ on this machine to build the fixture with"
else
    bundle_at "$SHELLTEST_TMP/static-looking"
    cat > "$SHELLTEST_TMP/fake-qt.cpp" <<'CPP'
// Not Qt. A class of the same name, so its mangled symbol is the one the check
// looks for -- which is the whole of what "Qt is inside this binary" means to nm.
class QCoreApplication
{
public:
    int exec();
};
int QCoreApplication::exec() { return 0; }
int main() { return 0; }
CPP
    g++ -rdynamic -o "$SHELLTEST_TMP/static-looking/usr/bin/mole" "$SHELLTEST_TMP/fake-qt.cpp"
    check_from_elsewhere "$SHELLTEST_TMP/static-looking/usr/bin/mole" "$SHELLTEST_TMP/static-looking"
    [ "$SCRIPT_STATUS" != 0 ] || fail "a binary defining QCoreApplication::exec() passed"
    said "Qt symbols are defined inside the binary"
    # And it survives stripping, which is the reason for reading .dynsym.
    if command -v strip >/dev/null 2>&1; then
        strip "$SHELLTEST_TMP/static-looking/usr/bin/mole"
        check_from_elsewhere "$SHELLTEST_TMP/static-looking/usr/bin/mole" \
            "$SHELLTEST_TMP/static-looking"
        said "Qt symbols are defined inside the binary"
    fi
fi

done_testing
