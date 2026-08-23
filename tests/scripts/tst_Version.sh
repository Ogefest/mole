#!/usr/bin/env bash
#
# The version is written down once, and what the binary prints is that number.
#
# It used to be written twice -- project(VERSION) in CMakeLists.txt and a string
# literal in src/app/main.cpp -- and the two agreed only because nobody had ever
# changed either. The first bump would have made `mole --version` a lie, and
# --version is how somebody with a bug report says which build they are holding,
# which is the whole reason it exists. See MOLE-117.
#
# Static where it can be: two of the three cases here read files and cost nothing.
# The third starts the binary, because "what it prints" is a claim about a process
# and cannot be checked from inside one.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

# The one line that holds it. Written as a pattern rather than a number so this
# test does not become the second copy it exists to prevent.
version_lines() { sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt; }

begin "the version is written down in exactly one place"
count=$(version_lines | grep -c .)
[ "$count" = 1 ] || fail "found $count VERSION lines in CMakeLists.txt; there has to be exactly one"
VERSION=$(version_lines)
printf '%s\n' "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || fail "'$VERSION' is not a version of three numbers"

# Nothing in the source spells one out. This is the rule that was broken, and it
# is the one that catches it coming back -- including in a plugin's own metadata,
# which carried the application's version three more times.
begin "no version is written a second time under src/"
: > "$SHELLTEST_TMP/literals"
grep -rnE '"[0-9]+\.[0-9]+\.[0-9]+"' src/ > "$SHELLTEST_TMP/literals" 2>/dev/null
if [ -s "$SHELLTEST_TMP/literals" ]; then
    fail "a version spelled out in the source cannot be kept true -- use MOLE_VERSION"
    sed 's/^/    /' "$SHELLTEST_TMP/literals"
fi
# The definition that makes that possible, so the rule above cannot be satisfied by
# deleting the version instead of by sourcing it.
#
# A dollar in a bracket rather than a backslash-dollar, throughout: the rule from
# MOLE-233 refuses deferred expansion in any script here, and it is right to --
# this is a pattern to match with, not a variable to expand later. Do not tidy the
# brackets away.
grep -q 'MOLE_VERSION="[$]{PROJECT_VERSION}"' CMakeLists.txt \
    || fail "CMakeLists.txt does not hand PROJECT_VERSION to the code"
grep -q 'QStringLiteral(MOLE_VERSION)' src/app/main.cpp \
    || fail "the application does not take its version from the build"

begin "the Makefile reads the version rather than spelling it out"
grep -qE '^VERSION := [$][(]shell .*CMakeLists[.]txt[)]$' Makefile \
    || fail "the Makefile does not read the version out of CMakeLists.txt"
# This version, and not any version: a path to an instrumented Qt legitimately
# carries Qt's own, and a rule that could not tell them apart would be a rule
# somebody turns off.
: > "$SHELLTEST_TMP/makefile-literals"
grep -nF "$VERSION" Makefile > "$SHELLTEST_TMP/makefile-literals"
if [ -s "$SHELLTEST_TMP/makefile-literals" ]; then
    fail "the Makefile spells out $VERSION, which is the third copy MOLE-117 was about"
    sed 's/^/    /' "$SHELLTEST_TMP/makefile-literals"
fi
said=$(make version 2>/dev/null | tail -1)
[ "$said" = "$VERSION" ] || fail "make version said '$said' and CMakeLists.txt says '$VERSION'"

begin "the binary prints the version it was configured with"
# MOLE_BINARY is set by the test's own environment in tests/CMakeLists.txt. A
# core-only build has no application at all, and a skip is reported rather than
# passed over quietly.
if [ -z "${MOLE_BINARY:-}" ] || [ ! -x "${MOLE_BINARY:-}" ]; then
    echo "  skipped: no application binary in this build (MOLE_BINARY=${MOLE_BINARY:-unset})"
else
    printed=$("$MOLE_BINARY" --version 2>/dev/null | head -1)
    [ "$printed" = "Mole $VERSION" ] \
        || fail "the binary said '$printed' and the build was configured with $VERSION"
fi

done_testing
