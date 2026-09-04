#!/usr/bin/env bash
#
# Every workflow in the repository, held to the two things all of them need, plus
# what the second-family job needs to be worth having.
#
# `tst_ReleaseWorkflow.sh` reads the release workflow closely, because what a
# pushed tag does is worth reading line by line. This file is the other half: a
# workflow nobody reads is a workflow that can stop parsing, or lose its trigger,
# and say nothing about it until somebody happens to look at the Actions tab. A
# scheduled job is the worst case for that -- nobody is waiting for its answer, so
# nobody notices its absence.
#
# The second-family cases are about a check being able to fail for the reason it
# exists. That job found four cases which cannot: they make a file unreadable and
# assert the failure is recorded, and a container job is root, which reads it
# anyway. They skip now when they cannot arrange what they need, so the job has to
# keep the two things that make them run rather than skip: an account that is not
# root, and no locale of its own. Both are one deletion away from being lost in
# silence -- the suite would still be green and four cases would quietly stop
# asserting anything. See MOLE-297 and ADR-0081.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

read_workflow() { python3 "$MOLE_SOURCE_DIR/tests/support/read-workflow.py" "$@"; }

begin "there are workflows to check at all"
# The guard on every case below: a glob that matches nothing passes everything.
workflows=$(find .github/workflows -name '*.yml' -o -name '*.yaml' | sort)
[ -n "$workflows" ] || fail "no workflow files were found, so nothing below asked anything"
count=$(printf '%s\n' "$workflows" | grep -c .)
[ "$count" -ge 2 ] || fail "only $count workflow file(s) found; the release one and the second family are both expected"

begin "every workflow parses"
for file in $workflows; do
    read_workflow "$file" parses > "$SHELLTEST_TMP/parse" 2>&1 \
        || { fail "$file is not valid YAML"; sed 's/^/    /' "$SHELLTEST_TMP/parse"; }
done

begin "every workflow says when it runs"
# A workflow with no trigger never runs and never says so.
for file in $workflows; do
    python3 - "$file" <<'PY' || fail "$file has nothing that starts it"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
# PyYAML reads the bare key `on` as the boolean True, which is a trap worth
# knowing about rather than working around: either spelling is the trigger.
triggers = document.get("on", document.get(True))
sys.exit(0 if triggers else 1)
PY
done

SECOND=.github/workflows/second-family.yml

begin "the second family runs on a clock and on demand"
[ -f "$SECOND" ] || fail "$SECOND is not there at all"
python3 - "$SECOND" <<'PY' || fail "the second family is not scheduled, or cannot be asked for by hand"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
triggers = document.get("on", document.get(True)) or {}
sys.exit(0 if "schedule" in triggers and "workflow_dispatch" in triggers else 1)
PY

begin "the second family builds and tests as an account that is not root"
# The four cases this is for skip themselves when the account can read a file with
# no permissions at all. Root can, and a container job is root.
python3 - "$SECOND" <<'PY' || fail "the build or the suite would run as root, where four cases cannot fail for their own reason"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
steps = [s for job in document["jobs"].values() for s in job["steps"]]
runs = "\n".join(s.get("run", "") for s in steps)
if "useradd" not in runs:
    sys.exit(1)
# The two that matter: whatever builds and whatever runs ctest must be handed to
# that account. Asked of the commands themselves rather than of the step names,
# because a step can be renamed.
for command in ("cmake --build", "ctest"):
    for line in runs.splitlines():
        if command in line and "runuser" not in line:
            sys.exit(1)
sys.exit(0)
PY

begin "the second family sets no locale, because that is the state it found a fault in"
# Natural ordering was byte ordering wherever the environment named no language.
# A LANG set here would have hidden it, and would hide the next one like it.
python3 - "$SECOND" <<'PY' || fail "the second family sets a locale, which is the machine state its own fault needed"
import sys, yaml
text = open(sys.argv[1], encoding="utf-8").read()
document = yaml.safe_load(text)
# In the environment of the job, of any step, or exported by a command in one.
places = [document.get("env") or {}]
for job in document["jobs"].values():
    places.append(job.get("env") or {})
    for step in job["steps"]:
        places.append(step.get("env") or {})
if any(key in place for place in places for key in ("LANG", "LC_ALL")):
    sys.exit(1)
runs = "\n".join(
    step.get("run", "") for job in document["jobs"].values() for step in job["steps"])
sys.exit(1 if ("LANG=" in runs or "LC_ALL=" in runs) else 0)
PY

FAST=.github/workflows/fast-tier.yml
RELEASE=.github/workflows/release.yml

begin "the fast tier runs on a push and on a pull request"
# The whole point of the file. A trigger deleted here puts the project back where it
# was on 2026-08-31: a broken build found by the weekly Fedora job up to seven days
# later, or by whoever next pushed a tag, mid-release. See MOLE-313.
[ -f "$FAST" ] || fail "$FAST is not there at all"
python3 - "$FAST" <<'PY' || fail "the fast tier does not run on a push, or not on a pull request"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
triggers = document.get("on", document.get(True)) or {}
sys.exit(0 if "push" in triggers and "pull_request" in triggers else 1)
PY

begin "the fast tier runs on the distribution the baseline Qt comes from"
# Ubuntu 24.04 ships Qt 6.4.2, which is what CMakeLists.txt requires and what the
# author develops against. A newer runner would quietly stop answering the question
# this job exists for, and would answer a different one nobody asked.
python3 - "$FAST" <<'PY' || fail "the fast tier does not run on ubuntu-24.04, so it is not testing the baseline"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
runners = {job.get("runs-on") for job in document["jobs"].values()}
sys.exit(0 if runners == {"ubuntu-24.04"} else 1)
PY

begin "the fast tier builds against the same libraries the release does"
# **A missing optional library does not go red, it goes quiet.** Every one of them is
# found QUIET behind a MOLE_HAVE_* flag, so a runner without it builds a smaller
# suite -- and a suite that skipped is a suite that asserted nothing. A job testing a
# leaner machine than the release builds on therefore misses exactly the breakages a
# release would meet, and looks green while doing it.
#
# The two lists are held equal rather than one being a subset of the other, so a
# library added to the release for the release's own reasons has to be a decision here
# as well. The exceptions are named in the script and nowhere else.
python3 - "$FAST" "$RELEASE" <<'PY' || fail "the fast tier and the release build against different libraries"
import sys, yaml

# Only the release needs these, and neither is a library: they unpack an .rpm so the
# licence check can be asked of it. The fast tier builds no artefact.
ONLY_THE_RELEASE_NEEDS = {"rpm", "cpio"}


def packages(path):
    document = yaml.safe_load(open(path, encoding="utf-8"))
    # The steps that install what the *build* needs, found by what they install
    # rather than by their names: Qt itself, and Arrow, which comes from Apache's
    # own repository in a step of its own. Named steps would be one rename away from
    # this rule quietly passing, and the release also installs packages for reasons
    # that are not the build's -- it puts its own .deb into a container and lets apt
    # pull the runtime libraries after it, and those are not a library this project
    # was compiled against.
    runs = "\n".join(
        step.get("run", "")
        for job in document["jobs"].values()
        for step in job["steps"]
        if "qt6-base-dev" in step.get("run", "") or "libarrow-dev" in step.get("run", ""))
    found, collecting = set(), False
    for line in runs.splitlines():
        stripped = line.strip()
        if "apt-get install" in stripped:
            collecting = True
            stripped = stripped.split("apt-get install", 1)[1]
        elif not collecting:
            continue
        continues = stripped.endswith("\\")
        for word in stripped.rstrip("\\").split():
            # Options, and the local .deb that adds Apache's own repository, which is
            # a path rather than a package name.
            if word.startswith(("-", ".", '"', "$", "'", "|", "&", ";")):
                continue
            found.add(word)
        if not continues:
            collecting = False
    return found


fast, release = packages(sys.argv[1]), packages(sys.argv[2])
missing = (release - ONLY_THE_RELEASE_NEEDS) - fast
extra = fast - release
if missing:
    print("  the release builds against these and the fast tier does not:", " ".join(sorted(missing)))
if extra:
    print("  the fast tier installs these and the release does not:", " ".join(sorted(extra)))
if missing or extra:
    sys.exit(1)
# And it found a list at all: an empty one would make the comparison vacuous, which is
# the failure mode of every test that compares two sets.
sys.exit(0 if len(fast) > 20 else 1)
PY

begin "the fast tier runs the fast tier and neither of the other two"
# MOLE-119: the release gate is the only place the live tiers will ever be a
# precondition of anything, because whatever runs this cannot reach the test
# environment. A tier added here is a tier that can never pass.
python3 - "$FAST" <<'PY' || fail "the fast tier reaches for a tier it cannot run, or has stopped excluding the heavy one"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
runs = "\n".join(
    step.get("run", "") for job in document["jobs"].values() for step in job["steps"])
if "test-live" in runs or "test-heavy" in runs:
    sys.exit(1)
# What runs the suite, and what makes it the fast tier rather than all of it.
sys.exit(0 if "ctest" in runs and "--label-exclude heavy" in runs else 1)
PY

begin "the fast tier says what the runner gave the build"
# Whoever reads a red run is looking at a green tree on their own machine, so what
# this runner did not have is the first thing worth knowing. Asked of the commands
# rather than of the step names, because a step can be renamed.
python3 - "$FAST" <<'PY' || fail "the fast tier does not print the configure summary, so a red run says less than it could"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
runs = "\n".join(
    step.get("run", "") for job in document["jobs"].values() for step in job["steps"])
sys.exit(0 if "configure.log" in runs and "Parquet" in runs else 1)
PY

begin "the fast tier publishes nothing"
# It runs on every push, so it is the workflow with the most chances to do something
# it should not. Nothing here writes to the repository, makes a tag or makes a
# release -- and a workflow that *cannot* is easier to be sure about than one that
# could and does not.
python3 - "$FAST" <<'PY' || fail "the fast tier could write to the repository, or tries to publish something"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
if (document.get("permissions") or {}).get("contents") != "read":
    sys.exit(1)
steps = [s for job in document["jobs"].values() for s in job["steps"]]
uses = " ".join(s.get("uses", "") for s in steps)
runs = "\n".join(s.get("run", "") for s in steps)
if "release" in uses.lower():
    sys.exit(1)
for forbidden in ("git push", "git tag", "gh release", "GITHUB_TOKEN"):
    if forbidden in runs:
        sys.exit(1)
sys.exit(0)
PY

begin "every workflow declares the shell its steps run in"
# A Linux runner's default is `bash -e {0}` -- `-e` without `-o pipefail`, so a
# pipeline takes the status of its last command and a step written as
# `something | tee log` cannot fail however `something` ended. release.yml carried
# a comment saying "there is no continue-on-error anywhere in this file", which
# was true of the keyword and not of the pipelines. `shell: bash` gets
# `bash --noprofile --norc -eo pipefail {0}`.
#
# windows.yml declares `pwsh` instead, and that is not an exception being waved
# through: that job is written in PowerShell -- backtick continuations,
# $env:GITHUB_ENV, Select-String -- which is the Windows runner's own default, so
# what it needed was the declaration and not a change. See MOLE-387.
for workflow in "$MOLE_SOURCE_DIR"/.github/workflows/*.yml; do
    name="$(basename "$workflow")"
    python3 - "$workflow" "$name" <<'PY' || fail "$name does not declare a shell with pipefail"
import sys, yaml
document = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
shell = ((document.get("defaults") or {}).get("run") or {}).get("shell")
wanted = "pwsh" if sys.argv[2] == "windows.yml" else "bash"
sys.exit(0 if shell == wanted else 1)
PY
done

done_testing
