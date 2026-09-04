#!/usr/bin/env python3
"""What the build's optional libraries are, read out of the CMake that finds them.

Every optional library goes through `mole_optional_dependency()`
(cmake/MoleOptionalDependency.cmake), which is the one place that knows the shape
of the line it prints. That makes the calls the list -- so the checks that used to
grep the CMake files for a message text read the calls instead, and a row added in
the right shape joins them by existing.

Three consumers: `tst_ReleaseWorkflow.sh`, which holds the release's refusal
strings against lines this build can actually print; `tst_Packages.sh`, which
holds ARCHITECTURE.md's table against the rows; and anybody asking what the
answer is on this machine.

Subcommands:
  lines    every line a row can print, one per line
  rows     NAME, SUMMARY, DEFINE, TARGET, SWITCH -- tab separated
  names    just the row names

See MOLE-390.
"""

import re
import sys
from pathlib import Path

# Where the calls are. Not a glob: a row in a file nobody reads is a row nobody
# checks, so the files are named and a new one has to be added here on purpose.
SOURCES = ("src/core/CMakeLists.txt", "src/plugins/CMakeLists.txt", "CMakeLists.txt")

CALL = "mole_optional_dependency("

KEYWORDS = (
    "SUMMARY", "VERSION", "VERSION_FROM", "FOUND", "MISSING", "MISSING_EXTRA",
    "TARGET", "SCOPE", "DEFINE", "SWITCH", "CACHE_PREFIX", "EXTRA_CONDITION",
    "PACKAGE", "QT_COMPONENT", "PKG_CONFIG", "LINK", "INCLUDE", "LINK_DIRS",
)


def calls(text):
    """Each `mole_optional_dependency(...)` call, as its argument text.

    Scanned rather than matched with one expression: an argument holds
    parentheses of its own -- "disabled (install libarrow-dev)" -- so a regex
    ending at the first `)` reads half a call. Quotes are tracked for the same
    reason.
    """
    position = 0
    while True:
        start = text.find(CALL, position)
        if start < 0:
            return
        cursor = start + len(CALL)
        depth = 1
        quoted = False
        while cursor < len(text) and depth:
            character = text[cursor]
            if character == '"' and text[cursor - 1] != "\\":
                quoted = not quoted
            elif not quoted and character == "(":
                depth += 1
            elif not quoted and character == ")":
                depth -= 1
            cursor += 1
        yield text[start + len(CALL):cursor - 1]
        position = cursor


def parse(arguments):
    """One call's arguments as {keyword: value}, plus "NAME" for the first one."""
    pieces = re.findall(r'"[^"]*"|\S+', arguments)
    pieces = [piece.strip('"') for piece in pieces]
    if not pieces:
        return None
    row = {"NAME": pieces[0]}
    keyword = None
    for piece in pieces[1:]:
        if piece in KEYWORDS:
            keyword = piece
            row.setdefault(keyword, "")
            continue
        if keyword is None:
            continue
        row[keyword] = (row[keyword] + " " + piece).strip() if row[keyword] else piece
    return row


def rows(root):
    found = []
    for name in SOURCES:
        path = root / name
        if not path.exists():
            continue
        for arguments in calls(path.read_text(encoding="utf-8")):
            row = parse(arguments)
            if row:
                row["FILE"] = name
                found.append(row)
    return found


def lines_of(row):
    """Every line this row can print, the way the helper composes them."""
    summary = row.get("SUMMARY", "")
    out = []
    for keyword in ("FOUND", "MISSING", "MISSING_EXTRA"):
        if row.get(keyword):
            out.append(f"{summary}: {row[keyword]}")
    if row.get("SWITCH"):
        out.append(f"{summary}: not built ({row['SWITCH']} is OFF)")
    return out


def main():
    root = Path(__file__).resolve().parents[2]
    question = sys.argv[1] if len(sys.argv) > 1 else "rows"
    found = rows(root)
    if not found:
        print("no mole_optional_dependency() calls found at all", file=sys.stderr)
        return 1

    if question == "lines":
        for row in found:
            for line in lines_of(row):
                print(line)
        return 0
    if question == "names":
        for row in found:
            print(row["NAME"])
        return 0
    if question == "rows":
        for row in found:
            print("\t".join([
                row["NAME"], row.get("SUMMARY", ""), row.get("DEFINE", ""),
                row.get("TARGET", ""), row.get("SWITCH", ""), row["FILE"],
            ]))
        return 0

    print(f"no such question: {question}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
