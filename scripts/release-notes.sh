#!/usr/bin/env bash
#
# The body of a release: the version's changelog block, and one line pointing at the
# table that says what the attached files are.
#
# **The block is the notes** -- MOLE-123 -- because notes that are taken cannot
# disagree with the changelog, and everything anybody has wanted in a release body
# since has turned out to be a fact that lives somewhere already. Sizes are on the
# page: GitHub prints one beside every attached file, so restating a hand-measured
# number would be a second copy of it. Capabilities are in README.md's table.
#
# **One line is the exception, and it is generated rather than written.** A release
# page shows four filenames and nothing that helps anybody choose between them, and
# the person who most needs to know the .deb has no Parquet grid is standing on that
# page. A line assembled from the tag has nothing to drift from; a sentence about
# what the .deb lacks, typed here, is the second list MOLE-123 exists to prevent and
# would be wrong the first time an artefact changed. See MOLE-303.
#
# The link is at the tag, so the table a reader follows is the one that describes the
# files they are looking at rather than whatever it says months later.
#
# Usage:
#   scripts/release-notes.sh <version> [<tag>] [<changelog>]
#
# GITHUB_REPOSITORY and GITHUB_REF_NAME are used when they are set, which is how the
# workflow passes them; otherwise the repository comes from the git remote and the
# tag from the version.
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

VERSION="${1:?usage: release-notes.sh <version> [tag] [changelog]}"
TAG="${2:-${GITHUB_REF_NAME:-v$VERSION}}"
CHANGELOG="${3:-$(cd "$HERE/.." && pwd)/CHANGELOG.md}"

REPOSITORY="${GITHUB_REPOSITORY:-}"
if [ -z "$REPOSITORY" ]; then
    # From the remote, in either of the two shapes a clone has.
    url=$(git -C "$HERE/.." remote get-url origin 2>/dev/null)
    REPOSITORY=$(printf '%s' "$url" | sed -e 's|^git@[^:]*:||' -e 's|^https://[^/]*/||' -e 's|\.git$||')
fi
[ -n "$REPOSITORY" ] || {
    echo "cannot tell which repository this is, so the line would point nowhere" >&2
    exit 1
}

# The block first, and its own refusal if there is not one: an empty block stops a
# release, and the line must not make an empty body look like a body with something
# in it. So nothing is printed at all unless the block is there.
block=$(bash "$HERE/changelog-block.sh" "$VERSION" "$CHANGELOG") || exit 1

printf '%s\n' "$block"
printf '\n'
printf 'What each of these files is, and what it can and cannot do: [the table in README.md](https://github.com/%s/blob/%s/README.md#getting-a-binary)\n' \
    "$REPOSITORY" "$TAG"
