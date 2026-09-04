#!/usr/bin/env python3
"""Answers structural questions about a GitHub workflow, for tst_ReleaseWorkflow.sh.

Here rather than inline in the shell test for the reason the deferred-expansion
checker is: the questions are about a nested document -- which step comes before
which, in which job, and whether anything is allowed to carry on past a failure --
and grep cannot answer those. A parser can, and PyYAML is already on the machine.

Each subcommand prints one thing and exits non-zero only when it cannot answer.
"""

import subprocess
import pathlib
import shlex
import sys
import tempfile
from pathlib import Path

import yaml


def load(path):
    with open(path, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def steps_of(document):
    """Every step of every job, in file order, with the job it belongs to."""
    for name, job in (document.get("jobs") or {}).items():
        for index, step in enumerate(job.get("steps") or []):
            yield name, index, step


def triggers(document):
    # `on:` parses as the boolean True in YAML 1.1, which is what PyYAML implements.
    section = document.get("on", document.get(True))
    if not isinstance(section, dict):
        return str(section)
    parts = []
    for event, filters in section.items():
        if not isinstance(filters, dict):
            parts.append(str(event))
            continue
        for kind, patterns in filters.items():
            parts.append(f"{event}.{kind}=" + ",".join(patterns))
    return " ".join(sorted(parts))


def order(document, first, second):
    """Whether the step running `first` comes before the one running `second`."""
    positions = {}
    for job, index, step in steps_of(document):
        script = step.get("run") or ""
        for needle in (first, second):
            if needle in script and needle not in positions:
                positions[needle] = (job, index)
    if first not in positions:
        return f"nothing runs {first!r}"
    if second not in positions:
        return f"nothing runs {second!r}"
    if positions[first][0] != positions[second][0]:
        return "in different jobs, so neither waits for the other"
    return "before" if positions[first][1] < positions[second][1] else "after"


# The four expressions that ask about job status. A step whose `if` calls one of
# them runs after a failure; a step whose `if` does not is evaluated only while the
# job is still succeeding, which is GitHub's own rule and the reason a condition on
# the event -- `github.event_name == 'push'` -- takes nothing away from "a red suite
# attaches nothing". Asked of the words rather than of the presence of an `if`,
# because the guard that keeps a dispatch from publishing is an `if`.
STATUS_FUNCTIONS = ("always(", "failure(", "cancelled(", "success(")


def bypasses(document):
    """Anything that would let the job get past a failing step.

    `continue-on-error` is the direct form; a condition that asks about job status
    is the indirect one -- `if: always()` publishes whatever the suite did.
    """
    found = []
    for job, index, step in steps_of(document):
        name = step.get("name") or step.get("uses") or f"step {index}"
        if step.get("continue-on-error"):
            found.append(f"{job}: {name} has continue-on-error")
        condition = str(step.get("if", ""))
        if any(function in condition for function in STATUS_FUNCTIONS):
            found.append(f"{job}: {name} runs regardless of failure, on: {condition}")
    for name, job in (document.get("jobs") or {}).items():
        if job.get("continue-on-error"):
            found.append(f"{name}: the job has continue-on-error")
    return "\n".join(found)


def condition(document, needle):
    """The `if` guarding the first step that mentions `needle`.

    "mentions" covers the three places a step can name what it does: the script it
    runs, the action it uses, and the arguments handed to that action. Prints an
    empty line when the step is unconditional, and says so when there is no such
    step -- an assertion about a guard must not pass because the step went away.
    """
    for _job, index, step in steps_of(document):
        haystack = "\n".join(
            [step.get("run") or "", step.get("uses") or ""]
            + [str(value) for value in (step.get("with") or {}).values()]
        )
        if needle in haystack:
            return str(step.get("if", ""))
    return f"nothing in this workflow mentions {needle!r}"


def summary_strings(document):
    """The configure-summary words a release build is checked against.

    Read out of scripts/feature-summary.sh rather than kept in a list here: that
    file is where the decision lives now, and a copy in the test would be the
    thing that goes stale.

    It used to be read out of two bash arrays inside the workflow -- and there
    were three copies of those arrays, one per consumer, two of which had already
    drifted. The `document` argument is kept so the question has the same shape as
    every other one here. See MOLE-387.
    """
    del document
    here = pathlib.Path(__file__).resolve().parents[2]
    text = (here / "scripts" / "feature-summary.sh").read_text(encoding="utf-8")

    wanted = []
    collecting = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped in ("MOLE_WANTED=()", "MOLE_REFUSED=()"):
            continue  # the declaration at the top of the file, which holds nothing
        if stripped.startswith(("MOLE_WANTED=(", "MOLE_REFUSED=(")):
            collecting = True
            continue
        if stripped.startswith(("MOLE_WANTED+=(", "MOLE_REFUSED+=(")):
            # A one-line append: everything between the parentheses.
            inside = stripped[stripped.index("(") + 1 : stripped.rindex(")")]
            wanted.extend(piece.strip('"') for piece in shlex.split(inside))
            continue
        if collecting:
            if stripped == ")":
                collecting = False
                continue
            if stripped and not stripped.startswith("#"):
                wanted.append(stripped.strip('"'))
    return "\n".join(wanted)


def shell_check(document):
    """Every run script, through `bash -n`."""
    problems = []
    for job, index, step in steps_of(document):
        script = step.get("run")
        if not script:
            continue
        name = step.get("name") or f"step {index}"
        with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as handle:
            handle.write(script)
            path = handle.name
        try:
            result = subprocess.run(
                ["bash", "-n", path], capture_output=True, text=True, check=False
            )
            if result.returncode != 0:
                problems.append(f"{job}: {name}: {result.stderr.strip()}")
        finally:
            Path(path).unlink()
    return "\n".join(problems)


def main():
    if len(sys.argv) < 3:
        print("usage: read-workflow.py <file> <question> [arguments]", file=sys.stderr)
        return 2
    path, question, arguments = sys.argv[1], sys.argv[2], sys.argv[3:]
    document = load(path)

    if question == "parses":
        return 0
    if question == "triggers":
        print(triggers(document))
        return 0
    if question == "jobs":
        print(" ".join(document.get("jobs") or {}))
        return 0
    if question == "order":
        print(order(document, arguments[0], arguments[1]))
        return 0
    if question == "condition":
        print(condition(document, arguments[0]))
        return 0
    if question == "bypasses":
        print(bypasses(document))
        return 0
    if question == "summary-strings":
        print(summary_strings(document))
        return 0
    if question == "shell-check":
        problems = shell_check(document)
        if problems:
            print(problems, file=sys.stderr)
            return 1
        return 0

    print(f"no such question: {question}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
