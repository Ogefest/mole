#!/usr/bin/env bash
#
# Prints one version's block out of CHANGELOG.md: its release notes.
#
# The notes already exist. Once the changelog is a structured log (ADR-0080), the
# entries for a version are exactly the lines between its marker and the next one,
# written by whoever landed the work rather than remembered at release time. The
# alternative is somebody writing them again by hand when the tag goes up, which is
# a second list of the same facts and will disagree with the first inside two
# releases. See MOLE-123.
#
# **By the expression the changelog's own header states**, read out of the file --
# not a second one here that means to say the same thing. That is the whole
# arrangement: one place holds the format, everything else asks it.
#
# **The oldest marker's block runs to the end of the file**, and the first release
# is the case that depends on it: 0.1.0's block is everything under its marker,
# including the prose that predates the format and belongs to it. An extractor
# written as "between two markers" finds nothing for the only marker there is.
#
# Usage:
#   scripts/changelog-block.sh 0.2.0          # from CHANGELOG.md
#   scripts/changelog-block.sh 0.2.0 <file>   # from another copy, for a test
#
set -uo pipefail

VERSION="${1:?usage: changelog-block.sh <version> [changelog]}"
CHANGELOG="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CHANGELOG.md}"

[ -f "$CHANGELOG" ] || {
    echo "no changelog at $CHANGELOG" >&2
    exit 1
}

# The shape, from the header, through the one reader of it -- which is also what
# took the GNU-only `sed -n "0,/^marker:/{...}"` out of here. See
# scripts/changelog-shape.sh.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/changelog-shape.sh"
marker_re=$(mole_changelog_shape marker "$CHANGELOG") || {
    echo "$CHANGELOG does not state its release-marker expression, so nothing can find a block" >&2
    exit 1
}

block=$(awk -v want="$VERSION" -v marker="$marker_re" '
    # Fenced blocks are skipped: the header shows an example of a marker, and an
    # example must not be mistaken for the release it describes.
    /^```/ { fence = !fence; next }
    fence  { next }
    $0 ~ marker {
        # A marker: this version starts the block, and the next marker ends it.
        # Without a next marker the block runs to the end of the file, which is what
        # the oldest release needs.
        if (inside) { inside = 0 }
        else if ($2 == want) { inside = 1; found = 1 }
        next
    }
    inside { print }
    END { if (!found) exit 3 }
' "$CHANGELOG")
status=$?

if [ "$status" = 3 ]; then
    cat >&2 <<MESSAGE
no release marker for $VERSION in $CHANGELOG.

The notes for a release are its block in that file, so there is nothing to publish.
Either the tag names a version that was never cut -- \`make release\` writes the
marker as it cuts -- or the marker is there in the wrong shape and the expression in
the file's own header does not match it.
MESSAGE
    exit 1
fi
[ "$status" = 0 ] || exit "$status"

# Blank lines at either end are the file's own spacing rather than content.
block=$(printf '%s\n' "$block" | sed -e '/./,$!d' | tac | sed -e '/./,$!d' | tac)

if [ -z "$block" ]; then
    cat >&2 <<MESSAGE
the block for $VERSION in $CHANGELOG is empty.

That is one of two things and both are worth stopping a release for: the marker is
in the wrong place -- above the entries it should be under, or below them all -- or
nothing was written down for anything that went into this release. The second is the
one worth checking first: an entry is a line per change, added as the change lands.
MESSAGE
    exit 1
fi

printf '%s\n' "$block"
