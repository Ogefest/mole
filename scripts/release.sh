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
#                      have written. The tiers still run: a dry run says whether a
#                      real one would go through, and that includes the live ones
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

# The live tiers print the address they are talking to. Theirs is their business;
# this script's output is the thing somebody pastes into a ticket or a release
# note, so the address does not pass through here. CLAUDE.md's rule about what may
# leave the environment directory, held rather than trusted.
redact() {
    if [ -n "${MOLE_TESTBED_ADDRESS:-}" ]; then
        sed "s|${MOLE_TESTBED_ADDRESS}|<the testbed>|g"
    else
        cat
    fi
}
# Without the colour, for reading rather than for showing.
plain() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
die() {
    printf '\nrefused: %s\n' "$1" >&2
    exit 1
}

# What this script has changed and has not yet committed. A failure anywhere after
# the first write puts all of it back: a half-cut release is worse than none,
# because the next run starts from a tree nobody has looked at.
mutating=0
PATHS="CMakeLists.txt CHANGELOG.md docs/guide/images"
SOURCE_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

# The tiers that need the live environment. This is the only place they will ever
# be a precondition of anything: they can only run from a machine that can reach
# that environment, and whatever publishes a release cannot. So everything needing
# it runs before the tag exists, and a pipeline re-runs what it can. See MOLE-119.
#
# There is no way round them. A gate with a documented bypass is a gate that gets
# gone around; if that turns out to be too strict, the ticket to loosen it can be
# written by somebody who has met the case.
run_tier() {
    local target="$1"
    local log="$SCRATCH/$target.log"

    step "the $target tier"
    say "$MAKE $target"
    "$MAKE" "$target" 2>&1 | tee "$log" | redact
    local code=${PIPESTATUS[0]}

    # A skip is not a pass, and this is where that has to be enforced rather than
    # assumed: test-live.sh already exits non-zero when a suite skipped, but the
    # heavy tier exits with its binaries' status and a QTest binary returns 0 for a
    # case it skipped. A destination with no room, or a control channel that is not
    # there, therefore looks exactly like success from the outside.
    #
    # Two shapes and both anchored, rather than "any line with SKIP in it": the
    # heavy tier prints the tail of its own report at the end, and a column in it
    # reading SKIPPED-none would have refused a perfectly good release.
    local skipped
    skipped=$(plain "$log" | sed -n \
        -e 's/^[[:space:]]*SKIP[[:space:]]*:[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' \
        -e 's/^[[:space:]]*SKIP[[:space:]]\{2,\}\(tst_[A-Za-z0-9_]*\).*/\1/p' \
        | sort -u | tr '\n' ' ')
    if [ -n "$skipped" ]; then
        die "$target skipped: $skipped -- a suite that never met the environment is not a pass"
    fi

    # 2 is what both tier scripts exit when the environment is not configured at
    # all. Named separately because it is the one failure that is about this
    # machine rather than about the code.
    if [ "$code" = 2 ]; then
        die "$target could not run: the live environment is not configured on this machine"
    fi
    [ "$code" = 0 ] || die "$target is not green"
    say "green, against the real environment"
}

run_tier test-live
run_tier test-heavy

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
# And \. to [.], because awk takes the expression as a dynamic regex and treats a
# backslash-dot as a plain dot -- which matches any character, so 0x1x0 would have
# been a version. It warns about it, too, on every run.
marker_re=${marker_re//\\./[.]}
entry_re=${entry_re//\\d/[0-9]}
entry_re=${entry_re//\\./[.]}

marker="## $next — released $(date +%F)"
printf '%s\n' "$marker" | grep -qE "$marker_re" \
    || die "the marker '$marker' is not the shape CHANGELOG.md states"
say "marker: $marker"

# The notes, before anything is written: they are this version's block in the
# changelog, and a release whose block is empty or missing is one nobody can publish.
# Checked here rather than discovered by the workflow after the tag is pushed, since
# a tag is the one step that cannot be taken back.
step "the release notes"
say "the block for $next in CHANGELOG.md, which is what the release will carry"

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

# And it has something under it. `make release` has just put the marker above the
# newest entry, so an empty block here means nothing was written down for anything in
# this release -- which the script says in those words rather than leaving a reader to
# find an empty release page. See MOLE-123.
# The changelog of the tree being released, named rather than defaulted: the script
# next door falls back to its own repository's copy, which is the wrong file when
# this is run anywhere else -- as its own test does.
notes=$("$SOURCE_SCRIPTS/changelog-block.sh" "$next" "$PWD/CHANGELOG.md" 2>&1) || die "$notes"
say "$(printf '%s\n' "$notes" | grep -c .) lines of notes for $next"

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
