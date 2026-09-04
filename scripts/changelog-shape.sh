#!/usr/bin/env bash
#
# The two expressions CHANGELOG.md states about itself, read out of the file.
#
# ADR-0080 put the entry and marker shapes in the changelog's own header so there
# is one copy and nothing can disagree with it. Two scripts then read them, and
# both did it their own way: `scripts/release.sh` with a portable `sed -n
# "s/^label: *//p" | head -1`, and `scripts/changelog-block.sh` with
# `sed -n "0,/^marker:/{...}"` -- whose `0,/re/` address is a GNU extension that
# BSD sed refuses, so the extractor worked on Linux and would have failed on macOS
# with "invalid usage of line address 0". Each also carried its own copy of the
# same two translations. One expression in the repository and two readers of it is
# most of a second copy. See MOLE-390 and ADR-0080.
#
# Sourced, not run:
#
#   . "$(dirname "$0")/changelog-shape.sh"
#   marker_re=$(mole_changelog_shape marker CHANGELOG.md) || exit 1
#
set -uo pipefail

# Prints the expression labelled <label> as one a POSIX ERE tool will take.
mole_changelog_shape() {
    local label="$1" file="$2" expression
    expression=$(sed -n "s/^$label: *//p" "$file" | head -1)
    [ -n "$expression" ] || return 1

    # The header writes \d, the way the expression reads everywhere it is quoted;
    # grep and awk want [0-9].
    expression=${expression//\\d/[0-9]}
    # And \. to [.], because awk takes the expression as a dynamic regex and
    # treats a backslash-dot as a plain dot -- which matches any character, so
    # 0x1x0 would have been a version. It warns about it, too, on every run.
    expression=${expression//\\./[.]}

    printf '%s\n' "$expression"
}
