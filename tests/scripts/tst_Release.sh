#!/usr/bin/env bash
#
# `make release` cuts one, and refuses to when anything is not as it should be.
#
# Driven against a repository this test builds: a temp work tree with a
# CMakeLists.txt, a CHANGELOG.md that states its own shapes, a Makefile carrying
# the three targets the script asks of the project -- `version`, `test` and
# `guide-images` -- and a bare repository standing in for the remote. So the
# commit, the annotated tag and the push are all real and none of them leaves the
# machine, and the suite the gate runs is a line in a fixture rather than the whole
# of Mole's.
#
# That the project's own Makefile answers `make version` is held by
# tst_Version.sh. What is under test here is the gate and the order of it. See
# MOLE-118.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

RELEASE="$MOLE_SOURCE_DIR/scripts/release.sh"
TODAY=$(date +%F)

# A repository ready to have a release cut from it. Called per case, because what
# most of these assert is that nothing was changed, and a shared fixture would
# carry one case's writes into the next.
fixture() {
    local repo="$SHELLTEST_TMP/repo"
    rm -rf "$repo" "$SHELLTEST_TMP/remote.git"
    mkdir -p "$repo/docs/guide/images"
    git init -q -b main "$repo"
    git init -q --bare "$SHELLTEST_TMP/remote.git"

    cat > "$repo/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)

project(Mole
    VERSION 0.4.0
    DESCRIPTION "Something to cut a release of"
    LANGUAGES CXX)
EOF

    cat > "$repo/CHANGELOG.md" <<'EOF'
# Changelog

Two shapes, and outside a fenced code block nothing else in this file may match
either of them:

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

2026-08-23 #MOLE-118 The newest thing that happened
2026-08-22 #MOLE-117 Something before it
EOF

    # The three targets the script asks of the project, and nothing else. `version`
    # reads the same line the real Makefile reads, because the script rewrites that
    # line and then asks again.
    cat > "$repo/Makefile" <<'EOF'
VERSION := $(shell sed -n 's/^ *VERSION \([0-9][0-9.]*\)$$/\1/p' CMakeLists.txt)

version:
	@echo "$(VERSION)"

test:
	@test ! -f fail-the-suite || { echo "a case failed"; exit 1; }
	@echo "suite green"

guide-images:
	@echo "a picture, taken at $$(date +%s%N)" > docs/guide/images/01-shot.png
	@echo "  guide images: 1 rewritten"
EOF

    echo "a picture" > "$repo/docs/guide/images/01-shot.png"

    git -C "$repo" config user.name "Test"
    git -C "$repo" config user.email "test@example.invalid"
    git -C "$repo" remote add origin "$SHELLTEST_TMP/remote.git"
    git -C "$repo" add -A
    git -C "$repo" commit -q -m "Everything before the release"
    git -C "$repo" push -q origin main
    printf '%s' "$repo"
}

# Runs the script in `repo` and keeps everything it said.
#
# Not called `cut`: that is a command, and a function named after one turns
# `grep -n ... | cut -d: -f1` into a call to this. Which is what happened, and cost
# an hour of looking at the wrong end of the pipe.
#
# It writes where the harness expects, so `said` and `exited` from shelltest.sh
# are what assert on it -- including their dump of the whole run on a failure.
cut_release() {
    local repo="$1"
    shift
    ( cd "$repo" && env "$@" bash "$RELEASE" ) > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

# The first line of the file matching a pattern, without a pipe into `head`.
line_of() { awk -v re="$2" '$0 ~ re { print NR; exit }' "$1"; }

# --------------------------------------------------------------- refusals

begin "a dirty tree is refused, and nothing is touched"
repo=$(fixture)
echo "work in progress" > "$repo/CMakeLists.txt.mine"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a dirty tree was accepted"
said "the tree is dirty"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"
[ "$(git -C "$repo" rev-list --count HEAD)" = 1 ] || fail "a commit was made anyway"

begin "a branch that releases are not cut from is refused"
repo=$(fixture)
git -C "$repo" checkout -q -b a-fix
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut off the branch"
said "releases are cut from main"

begin "a suite that is not green is refused, and nothing is touched"
repo=$(fixture)
touch "$repo/fail-the-suite"
git -C "$repo" add -A
git -C "$repo" commit -q -m "Arrange for the suite to fail"
before=$(git -C "$repo" rev-parse HEAD)
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a failing suite"
said "the suite is not green"
[ "$(git -C "$repo" rev-parse HEAD)" = "$before" ] || fail "it committed something"
[ -z "$(git -C "$repo" status --porcelain)" ] || fail "it left the tree dirty"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"

# ---------------------------------------------------------------- a dry run

begin "a dry run says what it would cut and writes nothing"
repo=$(fixture)
cut_release "$repo" DRY=1
[ "$SCRIPT_STATUS" = 0 ] || fail "a dry run failed"
said "cutting 0.4.0"
said "## 0.4.0 — released $TODAY"
said "dry run: nothing was written"
[ -z "$(git -C "$repo" status --porcelain)" ] || { fail "a dry run wrote something"; git -C "$repo" status --short; }
[ -z "$(git -C "$repo" tag --list)" ] || fail "a dry run made a tag"
grep -q '^## ' "$repo/CHANGELOG.md" && fail "a dry run wrote the marker into the file"

# ------------------------------------------------------------- a real cut

begin "the first cut is the version the code already claims"
repo=$(fixture)
cut_release "$repo"
[ "$SCRIPT_STATUS" = 0 ] || fail "the release did not go through"
said "the first release, which is the version the code already claims"
# One commit, and it carries all three things.
[ "$(git -C "$repo" rev-list --count HEAD)" = 2 ] || fail "expected exactly one release commit"
[ "$(git -C "$repo" log -1 --format=%s)" = "Release 0.4.0" ] || fail "the commit message does not name the version"
carried=$(git -C "$repo" show --name-only --format= HEAD | sort | tr '\n' ' ')
case "$carried" in
    *CHANGELOG.md*) ;;
    *) fail "the release commit does not carry the changelog: $carried" ;;
esac
case "$carried" in
    *docs/guide/images/01-shot.png*) ;;
    *) fail "the release commit does not carry the regenerated pictures: $carried" ;;
esac
# The marker, above the newest entry and nowhere else.
head -n 20 "$repo/CHANGELOG.md" | grep -qF "## 0.4.0 — released $TODAY" \
    || { fail "the marker is not near the top of the file"; head -n 20 "$repo/CHANGELOG.md" | sed 's/^/    /'; }
[ "$(grep -c '^## ' "$repo/CHANGELOG.md")" = 1 ] || fail "more than one marker was written"
marker_line=$(line_of "$repo/CHANGELOG.md" '^## ')
entry_line=$(line_of "$repo/CHANGELOG.md" '^2026-08-23')
[ "$marker_line" -lt "$entry_line" ] || fail "the marker went below the newest entry"
# An annotated tag, on that commit, and both are on the remote.
[ "$(git -C "$repo" cat-file -t v0.4.0)" = tag ] || fail "v0.4.0 is not an annotated tag"
[ "$(git -C "$repo" rev-list -n 1 v0.4.0)" = "$(git -C "$repo" rev-parse HEAD)" ] \
    || fail "the tag is not on the release commit"
[ "$(git -C "$SHELLTEST_TMP/remote.git" rev-parse main)" = "$(git -C "$repo" rev-parse HEAD)" ] \
    || fail "the commit was not pushed"
[ "$(git -C "$SHELLTEST_TMP/remote.git" cat-file -t v0.4.0)" = tag ] || fail "the tag was not pushed"
# The version is untouched by a first cut, because it is already the one being cut.
[ "$( (cd "$repo" && make version) )" = "0.4.0" ] || fail "the first cut changed the version"

begin "the next cut bumps the patch, and the overrides choose otherwise"
# Sequential on purpose: each cut has to put its marker above the last one, which
# is the case a fresh fixture cannot reach.
cut_release "$repo"
[ "$SCRIPT_STATUS" = 0 ] || fail "the second release did not go through"
said "a patch bump, which is the default"
[ "$( (cd "$repo" && make version) )" = "0.4.1" ] || fail "the patch was not bumped in CMakeLists.txt"
[ "$(git -C "$repo" cat-file -t v0.4.1)" = tag ] || fail "v0.4.1 is not an annotated tag"

cut_release "$repo" MINOR=1
[ "$SCRIPT_STATUS" = 0 ] || fail "MINOR=1 did not go through"
[ "$( (cd "$repo" && make version) )" = "0.5.0" ] || fail "MINOR=1 did not cut 0.5.0"

cut_release "$repo" MAJOR=1
[ "$SCRIPT_STATUS" = 0 ] || fail "MAJOR=1 did not go through"
[ "$( (cd "$repo" && make version) )" = "1.0.0" ] || fail "MAJOR=1 did not cut 1.0.0"

cut_release "$repo" VERSION=2.3.4
[ "$SCRIPT_STATUS" = 0 ] || fail "VERSION= did not go through"
[ "$( (cd "$repo" && make version) )" = "2.3.4" ] || fail "VERSION=2.3.4 did not cut 2.3.4"

begin "the markers stack newest first, and the file still holds nothing else"
# What the release notes are extracted from, so the order is the whole of it.
versions=$(grep '^## ' "$repo/CHANGELOG.md" | sed 's/^## \([0-9.]*\) .*/\1/' | tr '\n' ' ')
[ "$versions" = "2.3.4 1.0.0 0.5.0 0.4.1 0.4.0 " ] || fail "the markers are in the order: $versions"
# Every one of them is the shape the file states, checked with the file's own
# expression rather than a second copy of it.
marker_re=$(sed -n 's/^marker: *//p' "$repo/CHANGELOG.md" | head -1)
marker_re=${marker_re//\\d/[0-9]}
[ "$(grep -c '^## ' "$repo/CHANGELOG.md")" = "$(grep -cE "$marker_re" "$repo/CHANGELOG.md")" ] \
    || fail "a ## line in the file is not a release marker"

begin "a version that has already been cut is refused"
cut_release "$repo" VERSION=1.0.0
[ "$SCRIPT_STATUS" != 0 ] || fail "a version that is already a tag was cut again"
said "v1.0.0 is already a tag"
[ -z "$(git -C "$repo" status --porcelain)" ] || fail "it left the tree dirty"

done_testing
