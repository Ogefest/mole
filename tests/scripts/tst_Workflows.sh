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

done_testing
