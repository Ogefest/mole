#!/usr/bin/env bash
#
# What the machine gave the build, out of a configure log.
#
# Three workflows printed this and each had its own idea of how. Two carried the
# same hand-typed alternation -- "Parquet|Terminal|Git state|..." -- which names
# seven of the eleven optional libraries and silently stops mentioning the next
# one added. The third, windows.yml, asked `cmake -LH` for `MOLE_HAVE`, which is
# never listed: those are INTERNAL cache entries, so that step printed nothing at
# all on every run it ever made.
#
# The rows are read from the calls that print them, so a library added through
# mole_optional_dependency() appears here by existing. And a row that printed
# nothing is *named*, because that is the fault this file was written after: a
# summary step whose answer is silence looks exactly like a summary step with
# nothing to say.
#
# Usage:
#   scripts/configure-summary.sh <configure log>
#
# See MOLE-390 and cmake/MoleOptionalDependency.cmake.
#
set -uo pipefail

log="${1:-}"
[ -n "$log" ] || { echo "usage: scripts/configure-summary.sh <configure log>" >&2; exit 2; }
[ -f "$log" ] || { echo "configure-summary: no such log: $log" >&2; exit 2; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Anchored to the start of a line, because `SUMMARY` is also the tail of
# `CPACK_RPM_PACKAGE_SUMMARY`, which this read as a row whose line never printed.
# Every call writes one keyword per line; tests/support/read-optional-dependencies.py
# is the reader that understands the whole shape, and is what the suites use.
summaries=$(grep -HoE '^[[:blank:]]*SUMMARY "[^"]+"' \
    "$here/src/core/CMakeLists.txt" "$here/src/plugins/CMakeLists.txt" "$here/CMakeLists.txt" \
    2>/dev/null | sed 's/:[[:blank:]]*SUMMARY "/\t/; s/"$//')
[ -n "$summaries" ] || {
    echo "configure-summary: no optional-dependency rows found, so this printed nothing" >&2
    exit 1
}

# Grouped by the file the row is declared in, because a whole file can be out of
# a build: MOLE_CORE_ONLY=ON configures core and not the plugins, so the six rows
# in src/plugins/CMakeLists.txt say nothing and that is the build working. A file
# whose rows are *all* silent was not configured; one silent row among printed
# ones is the fault this script exists for.
silent_rows=0
silent_files=""
while IFS= read -r file; do
    [ -n "$file" ] || continue
    total=0
    quiet=0
    quiet_names=""
    while IFS=$'\t' read -r row_file summary; do
        [ "$row_file" = "$file" ] || continue
        total=$((total + 1))
        line=$(grep -F -- "$summary:" "$log" | tail -1)
        if [ -n "$line" ]; then
            # CMake prefixes its own status lines; the words are what matters here.
            printf '  %s\n' "${line#-- }"
        else
            quiet=$((quiet + 1))
            quiet_names="$quiet_names$summary"$'\n'
        fi
    done <<< "$summaries"

    if [ "$quiet" = 0 ]; then
        continue
    fi
    if [ "$quiet" = "$total" ]; then
        silent_files="$silent_files  (${file#"$here/"}: not configured in this build, so its $total row(s) said nothing)"$'\n'
        continue
    fi
    printf '%s' "$quiet_names" | while IFS= read -r summary; do
        [ -n "$summary" ] || continue
        printf '  %s: NOTHING PRINTED -- this row said nothing at configure time\n' "$summary"
    done
    silent_rows=$((silent_rows + quiet))
done <<< "$(printf '%s\n' "$summaries" | cut -f1 | uniq)"

[ -z "$silent_files" ] || printf '%s' "$silent_files"
[ "$silent_rows" = 0 ]
