#!/usr/bin/env python3
"""Answers structural questions about a GitHub workflow, for tst_ReleaseWorkflow.sh.

Here rather than inline in the shell test for the reason the deferred-expansion
checker is: the questions are about a nested document -- which step comes before
which, in which job, and whether anything is allowed to carry on past a failure --
and grep cannot answer those. A parser can, and PyYAML is already on the machine.

Each subcommand prints one thing and exits non-zero only when it cannot answer.
"""

import subprocess
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


def bypasses(document):
    """Anything that would let the job get past a failing step.

    `continue-on-error` is the direct form; a condition on a later step is the
    indirect one -- `if: always()` publishes whatever the suite did.
    """
    found = []
    for job, index, step in steps_of(document):
        name = step.get("name") or step.get("uses") or f"step {index}"
        if step.get("continue-on-error"):
            found.append(f"{job}: {name} has continue-on-error")
        if "if" in step:
            found.append(f"{job}: {name} is conditional on: {step['if']}")
    for name, job in (document.get("jobs") or {}).items():
        if job.get("continue-on-error"):
            found.append(f"{name}: the job has continue-on-error")
    return "\n".join(found)


def summary_strings(document):
    """The configure-summary words the workflow looks for.

    Read out of the two bash arrays rather than kept in a second list here: the
    workflow is where the decision lives, and a copy in the test would be the
    thing that goes stale.
    """
    wanted = []
    for _job, _index, step in steps_of(document):
        script = step.get("run") or ""
        collecting = False
        for line in script.splitlines():
            stripped = line.strip()
            if stripped in ("wanted=(", "refused=("):
                collecting = True
                continue
            if collecting:
                if stripped == ")":
                    collecting = False
                    continue
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
