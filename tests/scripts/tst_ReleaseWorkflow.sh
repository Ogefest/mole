#!/usr/bin/env bash
#
# What a pushed tag does, held by reading the workflow that does it.
#
# The end of this cannot be tested here and that is said out loud rather than
# papered over: whether a tag produces a release is a fact about GitHub's runners,
# and the only way to learn it is to push one. What *can* be held locally is
# everything that would make that run wrong, and most of it is structural --
# GitHub's own semantics say steps in a job run in order and a failed step ends the
# job, so "a red suite attaches nothing" follows from the publish step being after
# the test step in the same job with nothing allowed to carry on past a failure.
#
# The other half is drift. The workflow decides which optional libraries a release
# is built with -- an artefact without LibArchive silently has no archive browsing
# in it -- and it checks the configure summary for the words CMake prints. Both of
# those go stale silently: a new optional dependency nobody installs on the runner,
# or a reworded message that makes the check vacuous. Those are what the last two
# cases are for. See MOLE-120.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

WORKFLOW=.github/workflows/release.yml

# Reading YAML with a parser rather than with grep: the questions here are about
# structure -- which step is before which, and in the same job -- and grep cannot
# answer those about a nested document.
ask() { python3 "$MOLE_SOURCE_DIR/tests/support/read-workflow.py" "$WORKFLOW" "$@"; }

begin "there is a workflow and it parses"
[ -f "$WORKFLOW" ] || fail "$WORKFLOW is not there at all"
ask parses > "$SHELLTEST_TMP/parse" 2>&1 || {
    fail "$WORKFLOW is not valid YAML"
    sed 's/^/    /' "$SHELLTEST_TMP/parse"
}

begin "a pushed v tag is the only thing that starts it"
# The tag is the whole trigger, and `scripts/release.sh` is the only thing that
# makes a tag. A second way in -- a manual dispatch, a push to a branch -- would be
# a way to publish something the local gate never saw.
triggers=$(ask triggers)
[ "$triggers" = "push.tags=v*" ] || fail "it starts on: $triggers"

begin "nothing is published unless the suite has passed"
# One job, so nothing runs beside the tests; the publish step after the test step,
# so a red suite ends the job before anything is attached; and nothing anywhere
# that lets a step carry on past a failure.
jobs=$(ask jobs)
[ "$jobs" = "linux" ] || fail "expected one job, found: $jobs"

order=$(ask order 'make test' 'gh release create')
[ "$order" = "before" ] || fail "the suite does not run before the release is created: $order"

order=$(ask order 'make test' 'make bundle')
[ "$order" = "before" ] || fail "the suite does not run before the bundle is built: $order"

bypasses=$(ask bypasses)
[ -z "$bypasses" ] || fail "a step can carry on past a failure: $bypasses"

begin "every run script in it parses as shell"
# A workflow whose shell is broken fails at release time, on a machine nobody is
# watching, after the suite it just ran successfully.
ask shell-check > "$SHELLTEST_TMP/shell" 2>&1 || {
    fail "a step's script does not parse"
    sed 's/^/    /' "$SHELLTEST_TMP/shell"
}

begin "every optional library the build looks for is named in the workflow"
# The decision the ticket asks for: which optional libraries a release is built
# with. Every one of them has a configure message naming the package to install, so
# the list is derived from the build rather than kept here -- a new optional
# dependency joins this case by existing, which is the only way it stays true.
: > "$SHELLTEST_TMP/unnamed"
packages=$(grep -rhoE '\b(lib[a-z0-9]+-dev|qt6-[a-z]+-dev|qml6-module-[a-z]+)\b' \
           src/core/CMakeLists.txt src/plugins/CMakeLists.txt CMakeLists.txt | sort -u)
count=$(printf '%s\n' "$packages" | grep -c .)
[ "$count" -ge 8 ] || fail "only $count packages found in the build files; the parse has stopped working"
for package in $packages; do
    grep -qF "$package" "$WORKFLOW" || echo "$package" >> "$SHELLTEST_TMP/unnamed"
done
if [ -s "$SHELLTEST_TMP/unnamed" ]; then
    fail "the build looks for these and the workflow does not say what it does about them"
    sed 's/^/    /' "$SHELLTEST_TMP/unnamed"
fi

begin "the summary lines it checks for are lines the build can print"
# The workflow refuses to publish an artefact missing a feature by looking for the
# configure summary's own words. A reworded message would make every one of those
# searches quietly fail to match, which is a check that passes by finding nothing.
: > "$SHELLTEST_TMP/stale"
# The build's messages, joined: CMake wraps a long one across lines, so the text is
# read as one blob rather than line by line.
cat src/core/CMakeLists.txt src/plugins/CMakeLists.txt CMakeLists.txt \
    | tr '\n' ' ' | tr -s ' "' '  ' > "$SHELLTEST_TMP/messages"
lines=$(ask summary-strings)
count=$(printf '%s\n' "$lines" | grep -c .)
[ "$count" -ge 8 ] || fail "only $count summary strings found in the workflow; the parse has stopped working"
while IFS= read -r line; do
    [ -n "$line" ] || continue
    grep -qF "$line" "$SHELLTEST_TMP/messages" || echo "$line" >> "$SHELLTEST_TMP/stale"
done <<< "$lines"
if [ -s "$SHELLTEST_TMP/stale" ]; then
    fail "the workflow looks for words no configure message prints any more"
    sed 's/^/    /' "$SHELLTEST_TMP/stale"
fi

begin "every plugin that gets built also gets installed"
# The reading half of the workflow's own check. A plugin built and not installed is
# a feature the artefact silently has not got: the network one was built since it
# replaced rclone and never installed, so every `make bundle` had no sftp, ftp, s3
# or webdav drives in it. Derived from the plugin targets rather than a list, so the
# next plugin joins this case by existing.
: > "$SHELLTEST_TMP/uninstalled"
plugins=$(grep -ohE 'qt_add_plugin\(([a-z_]+)' src/plugins/CMakeLists.txt | sed 's/qt_add_plugin(//' | sort -u)
count=$(printf '%s\n' "$plugins" | grep -c .)
[ "$count" -ge 2 ] || fail "only $count plugin targets found; the parse has stopped working"
for plugin in $plugins; do
    grep -qE "install\(TARGETS $plugin\b" CMakeLists.txt || echo "$plugin" >> "$SHELLTEST_TMP/uninstalled"
done
if [ -s "$SHELLTEST_TMP/uninstalled" ]; then
    fail "these are built and never installed, so no artefact has them"
    sed 's/^/    /' "$SHELLTEST_TMP/uninstalled"
fi

begin "the artefact is named from the version rather than from a number"
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt)
[ -n "$version" ] || fail "cannot tell what version the repository is at"
grep -nF "$version" "$WORKFLOW" > "$SHELLTEST_TMP/hardcoded"
if [ -s "$SHELLTEST_TMP/hardcoded" ]; then
    fail "the workflow spells out $version; it should ask the build"
    sed 's/^/    /' "$SHELLTEST_TMP/hardcoded"
fi
grep -q 'make version' "$WORKFLOW" || fail "the workflow does not ask the build what version it is"

done_testing
