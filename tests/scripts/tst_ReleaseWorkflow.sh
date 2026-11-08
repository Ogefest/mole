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

begin "a pushed v tag, a push to main and a dispatch start it, and nothing else"
# The tag is the trigger that publishes, and `scripts/release.sh` is the only thing
# that makes a tag. The dispatch exists because a file of fourteen steps that had
# never executed would otherwise have had its first run be a real release -- see
# MOLE-314 -- and a push to main rehearses whenever the release path changes,
# because a dispatch nobody remembers is a rehearsal that does not happen -- see
# MOLE-436. Both are safe only because the publish step is guarded to a tag, which
# is the case below. A fourth way in, or either of them with that guard gone, would
# each be a way to publish something the local gate never saw.
triggers=$(ask triggers)
without_paths=$(tr ' ' '\n' <<<"$triggers" | grep -v '^push\.paths=' | paste -sd' ')
[ "$without_paths" = "push.branches=main push.tags=v* workflow_dispatch" ] \
    || fail "it starts on: $triggers"
# A push to main without a paths filter would rehearse on every commit: thirty-eight
# minutes of runner for a change to a comment in src/.
grep -q '^push\.paths=.' <<<"$(tr ' ' '\n' <<<"$triggers")" \
    || fail "a push to main is not filtered to the release path, so every commit rehearses"

begin "the release is published only from a tag, asked of the ref and not of the event"
# The whole safety of every rehearsal, in one condition. Asked of the step that
# creates the release rather than of a step name, because a step can be renamed.
#
# **It has to name the ref.** It read `github.event_name == 'push'`, which was
# right only while the one push trigger was a tag: with pushes to main rehearsing,
# that guard publishes a release from an ordinary commit. A comparison that is right
# by accident is the fault MOLE-435 fixed in another step. See MOLE-436.
guard=$(ask condition 'gh release create')
case "$guard" in
    *event_name*) fail "the step that creates the release asks about the event, and a push to main is a push: $guard" ;;
    *"!startsWith(github.ref, 'refs/tags/')"*) fail "the step that creates the release is guarded to everything but a tag: $guard" ;;
    *"startsWith(github.ref, 'refs/tags/')"*) ;;
    *) fail "the step that creates the release is guarded by: ${guard:-nothing at all}" ;;
esac

# And nothing in the file decides anything by the event: the tag check and the
# notes are each one push-to-main away from treating a rehearsal as a release.
# Comments may talk about it; conditions and commands may not.
by_event=$(grep -nE 'event_name|GITHUB_EVENT_NAME' "$WORKFLOW" | grep -vE '^[0-9]+:[[:space:]]*#')
[ -z "$by_event" ] || fail "the workflow still decides by the event: $by_event"

# And nothing else in the file publishes or tags. `gh release create` is the one
# command that makes a release; a `git push` or a `git tag` anywhere here would be a
# second way out that the guard above does not cover.
publishers=$(grep -c 'gh release create' "$WORKFLOW")
[ "$publishers" = 1 ] || fail "$publishers steps create a release; only the guarded one may"
forbidden=$(grep -nE '\bgit (push|tag)\b' "$WORKFLOW")
[ -z "$forbidden" ] || fail "the workflow pushes or tags: $forbidden"

begin "a rehearsal leaves the four artefacts behind"
# A rehearsal that only says "passed" cannot be looked at, and the ticket asks for
# each of the four to be started rather than merely produced -- which needs them
# downloadable. On anything but a tag: on a tag run they are on the release.
guard=$(ask condition 'upload-artifact')
case "$guard" in
    *"!startsWith(github.ref, 'refs/tags/')"*) ;;
    *) fail "nothing uploads the artefacts on a rehearsal; the guard reads: ${guard:-nothing at all}" ;;
esac
: > "$SHELLTEST_TMP/unuploaded"
for artefact in TARBALL '*.deb' '*.rpm' '*.AppImage'; do
    grep -qF "$artefact" "$WORKFLOW" || echo "$artefact" >> "$SHELLTEST_TMP/unuploaded"
done
if [ -s "$SHELLTEST_TMP/unuploaded" ]; then
    fail "a rehearsal would not leave all four artefacts behind; missing:"
    sed 's/^/    /' "$SHELLTEST_TMP/unuploaded"
fi

begin "nothing is published unless the suite has passed"
# One job, so nothing runs beside the tests; the publish step after the test step,
# so a red suite ends the job before anything is attached; and nothing anywhere
# that lets a step carry on past a failure.
#
# "Anywhere" means `continue-on-error`, or an `if` that asks about job status --
# `always()` and the three like it. An `if` that asks about the event does not: a
# step's condition is evaluated only while the job is still succeeding unless the
# expression itself asks otherwise, which is what lets the guard above exist without
# putting a hole in this.
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
#
# Read from the mole_optional_dependency() calls rather than by grepping the CMake
# files for the text: the message is composed there now -- a summary and one of
# three answers -- so no file holds the printed line as a contiguous string any
# more. tests/support/read-optional-dependencies.py puts them back together the
# way cmake/MoleOptionalDependency.cmake does. See MOLE-390.
: > "$SHELLTEST_TMP/stale"
python3 "$MOLE_SOURCE_DIR/tests/support/read-optional-dependencies.py" lines \
    > "$SHELLTEST_TMP/messages" \
    || fail "the optional-dependency calls could not be read at all"
rows=$(grep -c . "$SHELLTEST_TMP/messages")
[ "$rows" -ge 20 ] || fail "only $rows summary lines came back; the parse has stopped working"
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

begin "the plugins it compares are the ones the artefact was built from"
# **A step that could not pass, in the release path.** The check that every built
# plugin is in the artefact read `build/release/plugins` -- a directory the
# workflow *configures* to read the feature summary out of and never builds, so
# `built` was empty, `shipped` had two plugins, and the step failed whatever the
# artefact contained. It went in on 2026-09-04, after v0.1.2 was cut, so the tag
# for 0.1.3 was the first time it ever ran. See MOLE-435.
#
# Read out of the Makefile rather than spelled here, because two copies of a path
# is what this was: the recipe moved the build to `build/bundle` and the workflow
# went on naming the old one.
bundleDir=$(sed -n '/^bundle:/,/^[a-z]/p' Makefile \
    | sed -n 's/.*cmake --build \([^ ]*\).*/\1/p' | head -1)
[ -n "$bundleDir" ] || fail "cannot tell what directory the bundle target builds"
checked=$(grep -oE "find [^ ]*/plugins" "$WORKFLOW" | head -1 | sed 's/^find //; s|/plugins$||')
[ -n "$checked" ] || fail "the workflow no longer looks for built plugins at all"
[ "$checked" = "$bundleDir" ] \
    || fail "the workflow compares $checked/plugins against the artefact, but make bundle builds $bundleDir"

begin "an artefact with no plugins in it is a failure and not a match"
# Both sides empty compare equal, which is the failure class the commit that
# added this step was written to remove -- reproduced inside it. A release whose
# plugins all failed to build would have gone out green.
awk '/Every plugin that was built is in the artefact/,/^      - name: [^E]/' "$WORKFLOW" \
    > "$SHELLTEST_TMP/plugin-step"
grep -q 'built' "$SHELLTEST_TMP/plugin-step" || fail "cannot find the plugin comparison step"
# Bracket expressions rather than backslashes: a backslash next to a dollar in a
# tracked file is a finding of its own, and this line runs here. `[$]` is a
# literal dollar to an ERE and is not that sequence.
grep -qE '[-]z "[$]built"|[-]z "[$]shipped"|! [[] -s|count' "$SHELLTEST_TMP/plugin-step" \
    || fail "the step accepts an empty list of built plugins, which any two empty sides match"

begin "the artefact is named from the version rather than from a number"
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt)
[ -n "$version" ] || fail "cannot tell what version the repository is at"
grep -nF "$version" "$WORKFLOW" > "$SHELLTEST_TMP/hardcoded"
if [ -s "$SHELLTEST_TMP/hardcoded" ]; then
    fail "the workflow spells out $version; it should ask the build"
    sed 's/^/    /' "$SHELLTEST_TMP/hardcoded"
fi
grep -q 'make version' "$WORKFLOW" || fail "the workflow does not ask the build what version it is"

begin "the rehearsal's paths cover every script the release reaches"
# GitHub reads the filter literally, so it cannot be generated, and a filter that
# goes stale is a rehearsal that stops running without saying so. So the list of
# what a release reaches is derived here instead: every script a step runs, every
# make target it calls and that target's prerequisites, every script their
# recipes run, and every script those scripts name in turn. Each one has to fall
# inside the filter. See MOLE-436.
python3 - "$WORKFLOW" > "$SHELLTEST_TMP/uncovered" <<'PY'
import fnmatch, os, re, sys, yaml

document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
triggers = document.get("on", document.get(True)) or {}
patterns = (triggers.get("push") or {}).get("paths") or []
if not patterns:
    print("there is no paths filter at all")
    sys.exit(0)

runs = "\n".join(step.get("run") or "" for job in document["jobs"].values()
                 for step in job.get("steps") or [])
script = re.compile(r"scripts/[\w.-]+\.(?:sh|py)")

# The Makefile's rules, as target -> (prerequisites, recipe).
rules = {}
current = None
for line in open("Makefile", encoding="utf-8"):
    match = re.match(r"^([a-z][\w-]*):(?!=)(.*)$", line)
    if match:
        current = match.group(1)
        rules[current] = (match.group(2).split(), [])
    elif current and line.startswith("\t"):
        rules[current][1].append(line)
    elif line.strip() and not line.startswith("#"):
        current = None

reached = {sys.argv[1], "Makefile"}
reached.update(script.findall(runs))
targets = [t for t in re.findall(r"\bmake ([a-z][\w-]*)", runs) if t in rules]
seen = set()
while targets:
    target = targets.pop()
    if target in seen:
        continue
    seen.add(target)
    prerequisites, recipe = rules[target]
    targets.extend(p for p in prerequisites if p in rules)
    reached.update(script.findall("".join(recipe)))

# And what those scripts run in turn, to a fixed point.
pending = [path for path in reached if path.startswith("scripts/")]
while pending:
    path = pending.pop()
    if not os.path.isfile(path):
        continue
    for named in script.findall(open(path, encoding="utf-8").read()):
        if named not in reached:
            reached.add(named)
            pending.append(named)

if len([p for p in reached if p.startswith("scripts/")]) < 5:
    print(f"only {len(reached)} paths were derived, so the derivation found nothing")
for path in sorted(p for p in reached if os.path.exists(p)):
    if not any(fnmatch.fnmatch(path, pattern) for pattern in patterns):
        print(path)
PY
if [ -s "$SHELLTEST_TMP/uncovered" ]; then
    fail "a change to one of these would not rehearse the release:"
    sed 's/^/    /' "$SHELLTEST_TMP/uncovered"
fi

done_testing
