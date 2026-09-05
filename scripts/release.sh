#!/usr/bin/env bash
#
# Cuts a release: the gate, the version, the changelog marker, the manifest that
# says what the newest release is, one commit, an annotated tag, and the push.
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
#   DRY=1              everything except the version, the marker, the manifest,
#                      the commit, the tag and the push -- and it prints what it
#                      would have written. The tiers still run: a dry run says
#                      whether a real one would go through, and that includes the
#                      live ones
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
note() { printf '   %s\n' "$1"; }

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

# The file that says what the newest release is, and **the one decision here that
# can never be revisited**: its path. Every binary that ships from now on asks for
#
#   https://raw.githubusercontent.com/Ogefest/mole/main/latest.json
#
# for the rest of its life, and a binary released years earlier cannot be told that
# the file moved -- being told things is what the file is for. So it sits at the top
# of the repository: discoverable, and short enough to read out. See ADR-0084 and
# MOLE-323.
MANIFEST="latest.json"
PATHS="CMakeLists.txt CHANGELOG.md docs/guide/images $MANIFEST"
SOURCE_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mole-release.XXXXXX")
cleanup() {
    rm -rf "$SCRATCH"
    if [ "$mutating" = 1 ]; then
        printf '\n   putting the tree back\n'
        # One path at a time, rather than `git checkout -- $PATHS`: a checkout of
        # several paths is refused *whole* when any one of them is unknown to git,
        # and on the first release cut in a repository that has never had a
        # manifest that is the manifest. It would have put nothing back at all --
        # not the changelog, not the pictures.
        for path in $PATHS; do
            git checkout -- "$path" 2>/dev/null
        done
        # A file with a name nothing had before is not restored by a checkout: a
        # regenerated picture, and the manifest on the cut that introduces it.
        # Safe to remove: the gate refused anything untracked before writing began.
        git clean -fq docs/guide/images "$MANIFEST" 2>/dev/null
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
note "on $BRANCH, and the tree is clean"

# **What the remote thinks, asked before anything is written.**
#
# The gate checked the branch, the tree and the *local* tags, and nothing else --
# so if $REMOTE/$BRANCH had moved (a merged pull request, a commit from another
# clone), or if the tag existed there and not here, every step up to the push
# succeeded: the manifest, the changelog marker, the version line and the commit
# were written, the tag was made, the restore-on-failure was disarmed, and then
# the push was refused. Forty minutes of tiers, and a release commit and a tag to
# unpick by hand. ADR-0084 says a failed release "leaves the manifest alone" --
# true only of failures before the disarm, and the push is after the last write.
#
# Asked here, where a refusal costs nothing. A machine with no network is refused
# too, and that is right: a release that cannot be pushed is not a release, and
# finding out first is the whole point.
#
# Behind is the refusal, and ahead is not: a branch with commits the remote has
# not seen pushes them along with the release commit, which is the ordinary case
# and not a fault. What cannot be pushed is a branch missing commits the remote
# already has. See MOLE-388.
git fetch --quiet "$REMOTE" || die "cannot reach $REMOTE, and a release that cannot be pushed is not one"
if git rev-parse -q --verify "refs/remotes/$REMOTE/$BRANCH" >/dev/null; then
    behind=$(git rev-list --count "HEAD..$REMOTE/$BRANCH")
    [ "$behind" = 0 ] || die "$REMOTE/$BRANCH has $behind commit(s) this branch does not -- pull first, because the push at the end would be refused"
    ahead=$(git rev-list --count "$REMOTE/$BRANCH..HEAD")
    if [ "$ahead" = 0 ]; then
        note "level with $REMOTE/$BRANCH"
    else
        note "$ahead commit(s) ahead of $REMOTE/$BRANCH, which the push will carry"
    fi
else
    note "$REMOTE has no $BRANCH yet, so there is nothing to be behind"
fi

# Where the suite was built, which is where the tiers look. Asked of make because
# the Makefile is the one place that path is written -- `build/$(PRESET)` -- and a
# second copy of the rule here would be wrong the first time somebody cuts a
# release under another preset. Read the way the version is read below, for the
# same reason: a recursive make prints directory lines. See MOLE-328 and MOLE-321.
step "the build"
BUILD=$("$MAKE" --no-print-directory build-dir 2>/dev/null | grep -E '^[^ ]+/[^ ]+$' | tail -1)
[ -n "$BUILD" ] || die "the project does not say where it builds ($MAKE build-dir said nothing)"
note "$BUILD"

step "the test suite"
note "$MAKE test"
"$MAKE" test || die "the suite is not green"
note "green"

# The tiers that need the live environment. This is the only place they will ever
# be a precondition of anything: they can only run from a machine that can reach
# that environment, and whatever publishes a release cannot. So everything needing
# it runs before the tag exists, and a pipeline re-runs what it can. See MOLE-119.
#
# There is no way round them. A gate with a documented bypass is a gate that gets
# gone around; if that turns out to be too strict, the ticket to loosen it can be
# written by somebody who has met the case.
run_tier() {
    local tier="$1"
    local target="test-$tier"
    local script="scripts/testbed/$target.sh"
    local log="$SCRATCH/$target.log"

    step "the $target tier"
    note "$script $BUILD"
    bash "$script" "$BUILD" 2>&1 | tee "$log" | redact
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
    #
    # **Asked of the script rather than of make, and that is the whole of
    # MOLE-328.** This used to run `$MAKE test-live`, and make exits 2 for any
    # failure in a recipe -- so a tier that ran for seventy minutes against the
    # real machine and failed one case arrived here as 2 and was reported as an
    # environment nobody had configured, six lines under its own failure. The
    # distinction this check exists to draw was destroyed by the layer in front
    # of it, and it failed in the direction that sends a reader to their own
    # machine instead of to the fault. The scripts take the build directory as
    # their argument and say so themselves when it holds nothing built; make was
    # adding the exit code it flattened and nothing else. See MOLE-321 for the
    # neighbouring one, where make's "Leaving directory" chatter was read as the
    # version.
    if [ "$code" = 2 ]; then
        die "$target could not run: the live environment is not configured on this machine"
    fi
    [ "$code" = 0 ] || die "$target is not green"
    note "green, against the real environment"
}

run_tier live
run_tier heavy

step "the guide pictures"
if [ -n "$DRY" ]; then
    note "skipped in a dry run: it rewrites the pictures under docs/guide/images"
else
    # Armed before the step rather than after it: this is the first thing that
    # writes a tracked file, and a release that stops later must not leave
    # regenerated pictures behind. Found by the test, which cut a version that was
    # already tagged and then asked whether the tree had moved.
    mutating=1
    note "$MAKE guide-images"
    "$MAKE" guide-images || die "the guide pictures could not be regenerated"
fi

# ------------------------------------------------------------- the version

step "the version"
# **Asked for the line that is a version, not for the last line.** This script is
# itself run by make, so a `make` it invokes is a recursive one -- and make turns on
# --print-directory for those, so `make version | tail -1` came back with
# "make[1]: Leaving directory ..." and the gate refused a release for not knowing
# what version the repository was at. Both belts: the directory lines are switched
# off, and what is read is the line shaped like a version rather than whichever line
# happens to be last. See MOLE-321.
current=$("$MAKE" --no-print-directory version 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | tail -1)
grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' <<<"$current" \
    || die "the repository does not say what version it is at ($MAKE version said '$current')"
note "at $current"

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

grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' <<<"$next" || die "'$next' is not a version of three numbers"
git rev-parse -q --verify "refs/tags/v$next" >/dev/null && die "v$next is already a tag"

# The tag on the remote as well. Only an exact local duplicate was refused, so a
# tag pushed from another clone got all the way to the push -- see the note in
# the gate above.
if git ls-remote --exit-code --tags "$REMOTE" "v$next" >/dev/null 2>&1; then
    die "v$next is already a tag on $REMOTE, pushed from somewhere else"
fi

# And it has to be *newer*. Only exact duplication was refused, so a VERSION=
# below the current version, or below the newest tag, was accepted -- and would
# then write a latest.json naming an older release as the newest thing there is.
# sort -V rather than a numeric comparison per part: it is the same ordering the
# rest of the world uses for these, and 0.10.0 sorts above 0.9.0 in it.
newest="$current"
if [ -n "$tags" ]; then
    highest=$(printf '%s\n' "$tags" | sed 's/^v//' | sort -V | tail -1)
    [ -z "$highest" ] || newest=$(printf '%s\n%s\n' "$current" "$highest" | sort -V | tail -1)
fi
if [ "$next" != "$newest" ] && [ "$(printf '%s\n%s\n' "$next" "$newest" | sort -V | tail -1)" != "$next" ]; then
    die "$next is not above $newest, and a release below the newest one would publish a latest.json naming an older version"
fi

note "cutting $next -- $decided"

# ------------------------------------------------------------- the marker

step "the changelog"
# The shapes come out of CHANGELOG.md's own header, which is the only place they
# are written down -- see ADR-0080. So the marker this builds is checked against
# the file's rule rather than against a second copy of it, which is what
# tests/scripts/tst_Changelog.sh will hold it to afterwards.
# Through the one reader of them, which is where the translation from what the
# header writes to what grep and awk take now lives -- there were two copies of it.
# See scripts/changelog-shape.sh.
. "$SOURCE_SCRIPTS/changelog-shape.sh"
marker_re=$(mole_changelog_shape marker CHANGELOG.md) || marker_re=""
entry_re=$(mole_changelog_shape entry CHANGELOG.md) || entry_re=""
[ -n "$marker_re" ] && [ -n "$entry_re" ] || die "CHANGELOG.md does not state its own shapes"

# One reading of the clock, for the marker and for the manifest, so a cut that
# crosses midnight cannot date the two of them differently.
today=$(date +%F)
marker="## $next — released $today"
grep -qE "$marker_re" <<<"$marker" \
    || die "the marker '$marker' is not the shape CHANGELOG.md states"
note "marker: $marker"

# The notes, before anything is written: they are this version's block in the
# changelog, and a release whose block is empty or missing is one nobody can publish.
# Checked here rather than discovered by the workflow after the tag is pushed, since
# a tag is the one step that cannot be taken back.
step "the release notes"
note "the block for $next in CHANGELOG.md, which is what the release will carry"

# ------------------------------------------------------------ the manifest

# What a running Mole asks, and until this file existed there was no answer: a
# release page states the version in HTML and `git tag` states it to somebody with a
# checkout, and neither is something a program can read. See MOLE-323 and ADR-0084.
#
# `format` is what lets the file grow later without breaking a binary released years
# earlier: a field may be added, and never renamed or removed. `url` is the landing
# page, and the application opens what it is handed rather than assembling anything
# -- so pointing it at a real page one day is an edit to this file and no change to
# any copy of Mole already installed.
#
# Nothing here can produce a document that does not parse: `next` has been held
# against the version expression above, `today` is `date`'s own, and the URL is
# built out of `next`. That is what makes the template safe rather than lucky, and
# tst_Release.sh parses what comes out of it in both a dry run and a real one.
step "the manifest"
manifest=$(cat <<JSON
{
  "format": 1,
  "version": "$next",
  "released": "$today",
  "url": "https://github.com/Ogefest/mole/releases/tag/v$next"
}
JSON
)
note "$MANIFEST, and what this cut writes into it:"
printf '%s\n' "$manifest" | sed 's/^/   /'

if [ -n "$DRY" ]; then
    step "what a real run would do next"
    note "rewrite project(VERSION $current) to $next in CMakeLists.txt"
    note "insert the marker above the newest entry in CHANGELOG.md"
    note "write that manifest to $MANIFEST"
    note "commit those with the regenerated pictures, tag v$next, and push both to $REMOTE"
    printf '\ndry run: nothing was written\n'
    exit 0
fi

mutating=1

# The manifest, first of the writes and silent, because the step above printed the
# whole of it. It describes the release that has been *published*, and between this
# line and the push at the foot of this script it names one that does not exist yet.
# That window is a few seconds inside one script, and saying so here is the right
# amount of engineering for it.
printf '%s\n' "$manifest" > "$MANIFEST" || die "could not write $MANIFEST"

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
note "marker written above the newest entry"

# And it has something under it. `make release` has just put the marker above the
# newest entry, so an empty block here means nothing was written down for anything in
# this release -- which the script says in those words rather than leaving a reader to
# find an empty release page. See MOLE-123.
# The changelog of the tree being released, named rather than defaulted: the script
# next door falls back to its own repository's copy, which is the wrong file when
# this is run anywhere else -- as its own test does.
notes=$("$SOURCE_SCRIPTS/changelog-block.sh" "$next" "$PWD/CHANGELOG.md" 2>&1) || die "$notes"
note "$(printf '%s\n' "$notes" | grep -c .) lines of notes for $next"

# ------------------------------------------------------------ the version line

step "the version line"
# One line, and MOLE-117 made it the only place the number is written down.
sed -i "s/^\( *VERSION \)[0-9][0-9.]*$/\1$next/" CMakeLists.txt
wrote=$("$MAKE" --no-print-directory version 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | tail -1)
[ "$wrote" = "$next" ] || die "CMakeLists.txt still says $wrote after being rewritten"
note "project(VERSION $next)"

# ------------------------------------------------------------ commit and tag

step "the commit"
# shellcheck disable=SC2086
git add $PATHS
# Everything $PATHS carries, named: the message listed three of the four and left
# out latest.json, which is the file the updater reads and the one a reader of
# this commit is most likely to be looking for. See MOLE-390.
git commit --quiet -m "Release $next" \
    -m "The version, the changelog marker for $next, $MANIFEST as the updater will read it, and the guide pictures as they are at this commit." \
    || die "the commit failed"
mutating=0
note "$(git log --oneline -1)"

step "the tag"
git tag -a "v$next" -m "Mole $next" || die "the tag failed"
note "v$next, annotated, on $(git rev-parse --short HEAD)"

step "the push"
git push --quiet "$REMOTE" "$BRANCH" || die "the commit is made and tagged locally, but the push failed"
git push --quiet "$REMOTE" "v$next" || die "the commit and tag are local; pushing the tag failed"
note "pushed $BRANCH and v$next to $REMOTE"

printf '\nMole %s is cut.\n' "$next"
