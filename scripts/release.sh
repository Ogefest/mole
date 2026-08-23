#!/usr/bin/env bash
#
# Cuts a release: the gate, the version, the changelog marker, one commit, an
# annotated tag, and the push.
#
# `git tag` printed nothing until this existed. The tag is the whole trigger for
# whatever publishes a release, so this is deliberately the only thing in the
# repository that makes one -- a second way to tag is a second way to publish
# something nothing gated. See MOLE-118.
#
# Every step says what it is doing and what it decided, because the first run of
# this must not be the first time anybody sees its output. It stops at the first
# step that fails and puts the tree back as it was.
#
# Overrides, all through the environment so `make release FOO=1` reaches them:
#
#   DRY=1              everything except the version, the marker, the commit,
#                      the tag and the push -- and it prints the two it would
#                      have written
#   MAJOR=1  MINOR=1   bump that part instead of the patch
#   VERSION=x.y.z      cut exactly this version
#   BRANCH=  REMOTE=   where releases are cut from and pushed to
#
set -uo pipefail

BRANCH="${BRANCH:-main}"
REMOTE="${REMOTE:-origin}"
DRY="${DRY:-}"
MAKE="${MAKE:-make}"
MAJOR="${MAJOR:-}"
MINOR="${MINOR:-}"
VERSION="${VERSION:-}"

step() { printf '\n== %s\n' "$1"; }
say() { printf '   %s\n' "$1"; }
die() {
    printf '\nrefused: %s\n' "$1" >&2
    exit 1
}

# What this script has changed and has not yet committed. A failure anywhere after
# the first write puts all of it back: a half-cut release is worse than none,
# because the next run starts from a tree nobody has looked at.
mutating=0
PATHS="CMakeLists.txt CHANGELOG.md docs/guide/images"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mole-release.XXXXXX")
cleanup() {
    rm -rf "$SCRATCH"
    if [ "$mutating" = 1 ]; then
        printf '\n   putting the tree back\n'
        # shellcheck disable=SC2086
        git checkout -- $PATHS 2>/dev/null
        # A picture with a name nothing had before is not restored by a checkout.
        # Safe to remove: the gate refused anything untracked before writing began.
        git clean -fq docs/guide/images 2>/dev/null
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- the gate

step "the tree"
[ -f CMakeLists.txt ] || die "run this from the top of the repository"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "this is not a git work tree"

on=$(git rev-parse --abbrev-ref HEAD)
[ "$on" = "$BRANCH" ] || die "on $on, and releases are cut from $BRANCH (override with BRANCH=)"

# A release commit that swallowed somebody's work in progress cannot be taken
# apart afterwards: the version, the marker and the pictures are in it too.
[ -z "$(git status --porcelain)" ] || die "the tree is dirty, and a release commit must carry only its own changes"
say "on $BRANCH, and the tree is clean"

step "the test suite"
say "$MAKE test"
"$MAKE" test || die "the suite is not green"
say "green"

step "the live tiers"
# The hook, and it is empty on purpose: the tiers that need a server are added by
# MOLE-119. Said out loud rather than left absent, because a gate with a silent
# gap in it is a gate nobody can reason about.
say "skipped: nothing here yet -- the live tiers are MOLE-119, and this is where they go"

step "the guide pictures"
if [ -n "$DRY" ]; then
    say "skipped in a dry run: it rewrites the pictures under docs/guide/images"
else
    # Armed before the step rather than after it: this is the first thing that
    # writes a tracked file, and a release that stops later must not leave
    # regenerated pictures behind. Found by the test, which cut a version that was
    # already tagged and then asked whether the tree had moved.
    mutating=1
    say "$MAKE guide-images"
    "$MAKE" guide-images || die "the guide pictures could not be regenerated"
fi

# ------------------------------------------------------------- the version

step "the version"
current=$("$MAKE" version | tail -1)
printf '%s\n' "$current" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "the repository does not say what version it is at ($MAKE version said '$current')"
say "at $current"

tags=$(git tag --list 'v*')
if [ -n "$VERSION" ]; then
    next="$VERSION"
    decided="asked for with VERSION="
elif [ -z "$tags" ]; then
    # Nothing has ever been released, so the first cut is the version the code
    # already claims rather than one past it.
    next="$current"
    decided="the first release, which is the version the code already claims"
else
    IFS=. read -r part_major part_minor part_patch <<< "$current"
    if [ -n "$MAJOR" ]; then
        next="$((part_major + 1)).0.0"
        decided="a major bump"
    elif [ -n "$MINOR" ]; then
        next="$part_major.$((part_minor + 1)).0"
        decided="a minor bump"
    else
        next="$part_major.$part_minor.$((part_patch + 1))"
        decided="a patch bump, which is the default"
    fi
fi

printf '%s\n' "$next" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' || die "'$next' is not a version of three numbers"
git rev-parse -q --verify "refs/tags/v$next" >/dev/null && die "v$next is already a tag"
say "cutting $next -- $decided"

# ------------------------------------------------------------- the marker

step "the changelog"
# The shapes come out of CHANGELOG.md's own header, which is the only place they
# are written down -- see ADR-0080. So the marker this builds is checked against
# the file's rule rather than against a second copy of it, which is what
# tests/scripts/tst_Changelog.sh will hold it to afterwards.
shape() { sed -n "s/^$1: *//p" CHANGELOG.md | head -1; }
marker_re=$(shape marker)
entry_re=$(shape entry)
[ -n "$marker_re" ] && [ -n "$entry_re" ] || die "CHANGELOG.md does not state its own shapes"
# The header writes \d, the way the expression reads everywhere it is quoted;
# grep and awk want [0-9]. The same one translation the test makes.
marker_re=${marker_re//\\d/[0-9]}
entry_re=${entry_re//\\d/[0-9]}

marker="## $next — released $(date +%F)"
printf '%s\n' "$marker" | grep -qE "$marker_re" \
    || die "the marker '$marker' is not the shape CHANGELOG.md states"
say "marker: $marker"

if [ -n "$DRY" ]; then
    step "what a real run would do next"
    say "rewrite project(VERSION $current) to $next in CMakeLists.txt"
    say "insert the marker above the newest entry in CHANGELOG.md"
    say "commit those with the regenerated pictures, tag v$next, and push both to $REMOTE"
    printf '\ndry run: nothing was written\n'
    exit 0
fi

mutating=1

# At the top of the log, which means above the first entry *or marker*, whichever
# comes first. Above the newest entry alone is not enough and the test caught it:
# with nothing new written since the last release, the newest entry sits under the
# last marker, so a second cut put its marker underneath -- claiming the previous
# release's entries and leaving that release with none. Fenced blocks are skipped,
# because the header shows an example of each shape.
awk -v marker="$marker" -v entry="$entry_re" -v release="$marker_re" '
    /^```/ { fence = !fence }
    !placed && !fence && ($0 ~ entry || $0 ~ release) { print marker; print ""; placed = 1 }
    { print }
    END { if (!placed) exit 3 }
' CHANGELOG.md > "$SCRATCH/changelog" 2>/dev/null \
    || die "could not find an entry in CHANGELOG.md to put the marker above"
cat "$SCRATCH/changelog" > CHANGELOG.md
say "marker written above the newest entry"

# ------------------------------------------------------------ the version line

step "the version line"
# One line, and MOLE-117 made it the only place the number is written down.
sed -i "s/^\( *VERSION \)[0-9][0-9.]*$/\1$next/" CMakeLists.txt
wrote=$("$MAKE" version | tail -1)
[ "$wrote" = "$next" ] || die "CMakeLists.txt still says $wrote after being rewritten"
say "project(VERSION $next)"

# ------------------------------------------------------------ commit and tag

step "the commit"
# shellcheck disable=SC2086
git add $PATHS
git commit --quiet -m "Release $next" -m "The version, the changelog marker for $next, and the guide pictures as they are at this commit." \
    || die "the commit failed"
mutating=0
say "$(git log --oneline -1)"

step "the tag"
git tag -a "v$next" -m "Mole $next" || die "the tag failed"
say "v$next, annotated, on $(git rev-parse --short HEAD)"

step "the push"
git push --quiet "$REMOTE" "$BRANCH" || die "the commit is made and tagged locally, but the push failed"
git push --quiet "$REMOTE" "v$next" || die "the commit and tag are local; pushing the tag failed"
say "pushed $BRANCH and v$next to $REMOTE"

printf '\nMole %s is cut.\n' "$next"
