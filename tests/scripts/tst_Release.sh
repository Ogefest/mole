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
# The manifest -- `latest.json`, the machine-readable statement of what the newest
# release is -- is cut by the same script and so is asserted here too: that a real
# cut writes it into the release commit naming what was cut, that a dry run prints
# it and writes nothing, that a field may be added to it, and that a cut which
# stops afterwards puts it back. See MOLE-323 and ADR-0084.
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

# Where this project builds, which the gate asks for because it runs the tiers
# itself. See MOLE-328.
build-dir:
	@echo "build/debug"

guide-images:
	@echo "a picture, taken at $$(date +%s%N)" > docs/guide/images/01-shot.png
	@echo "  guide images: 1 rewritten"
EOF

    echo "a picture" > "$repo/docs/guide/images/01-shot.png"

    # **The two tiers, as scripts rather than as make targets**, because that is
    # how the gate runs them now: make exits 2 for any failure in a recipe, so a
    # tier that ran and failed was reported as an environment nobody had
    # configured. They answer the way the real ones do -- 2 when the environment
    # is not configured, a SKIP line when a suite never met it, and an ordinary
    # non-zero when a suite ran and failed. See MOLE-328.
    mkdir -p "$repo/scripts/testbed"
    cat > "$repo/scripts/testbed/test-live.sh" <<'EOF'
#!/usr/bin/env bash
test ! -f no-environment || { echo "Set MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD."; exit 2; }
test ! -f live-skips || { printf "  \033[33mSKIP\033[0m    tst_SftpFileSystem  Totals: 0 passed\n"; exit 1; }
test ! -f live-fails || {
    echo "Live suites against a-machine.invalid"
    printf "  \033[31mFAIL\033[0m    tst_SftpFileSystem       22 passed, 1 failed, 0 skipped\n"
    printf "          FAIL!  : TestSftpFileSystem::aKilledUploadLeavesNothingThatLooksFinished()\n"
    exit 1
}
echo "Live suites against a-machine.invalid"
echo "  6 ran, 0 skipped, 0 failed"
EOF
    cat > "$repo/scripts/testbed/test-heavy.sh" <<'EOF'
#!/usr/bin/env bash
test ! -f no-environment || { echo "Set MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD."; exit 2; }
test ! -f heavy-skips || { echo "SKIP   : TestHeavyTransfers::aTenGigabyteCopy() no room at the far end"; exit 0; }
test ! -f heavy-fails || { echo "FAIL!  : TestHeavyTransfers::aTenGigabyteCopy() the copy stopped short"; exit 1; }
echo "heavy tier green"
echo "recorded in the report:"
echo "  10 GiB copy    SKIPPED-none    412 MiB/s"
EOF
    chmod +x "$repo/scripts/testbed/test-live.sh" "$repo/scripts/testbed/test-heavy.sh"

    git -C "$repo" config user.name "Test"
    git -C "$repo" config user.email "test@example.invalid"
    git -C "$repo" remote add origin "$SHELLTEST_TMP/remote.git"
    git -C "$repo" add -A
    git -C "$repo" commit -q -m "Everything before the release"
    git -C "$repo" push -q origin main
    printf '%s' "$repo"
}

# One entry at the top of the log, as landing a change would leave it, and committed
# so the tree is clean for the next cut. Each release needs one: a marker with
# nothing under it is an empty block and the gate refuses it -- either the marker is
# misplaced or nobody wrote a line for anything that went in. See MOLE-123.
add_entry() {
    local repo="$1" text="$2"
    # At the top of the log, which means above the first entry *or marker* -- the
    # same place the release script puts a marker, and where landing a change leaves
    # a line. Above the newest entry alone would put it inside the last release.
    awk -v line="$text" '
        /^```$/ { fence = !fence; print; next }
        !placed && !fence && (/^2026-/ || /^## /) { print line; print ""; placed = 1 }
        { print }
    ' "$repo/CHANGELOG.md" > "$repo/CHANGELOG.next"
    mv "$repo/CHANGELOG.next" "$repo/CHANGELOG.md"
    git -C "$repo" add CHANGELOG.md
    git -C "$repo" commit -q -m "Something landed"
}

# Runs the script in `repo` and keeps everything it said.
#
# Not called `cut`: that is a command, and a function named after one turns
# `grep -n ... | cut -d: -f1` into a call to this. Which is what happened, and cost
# an hour of looking at the wrong end of the pipe.
#
# It writes where the harness expects, so `said` and `exited` from shelltest.sh
# are what assert on it -- including their dump of the whole run on a failure.
#
# **The script's own overrides are cleared before each run**, so what reaches it is
# what the case asked for and nothing else. They all arrive through the environment
# -- that is how `make release VERSION=x` reaches them -- and make exports a
# command-line variable to everything it runs, including the suite the release gate
# runs before it cuts. So `make release VERSION=0.1.0` handed `VERSION=0.1.0` to
# this file's own fake repository, five cases cut that instead of what they asked
# for, and the gate refused the release on its own test. Found on 2026-09-01 by the
# first real use of `make release`. See MOLE-319.
cut_release() {
    local repo="$1"
    shift
    ( cd "$repo" && env -u VERSION -u MAJOR -u MINOR -u DRY -u BRANCH -u REMOTE \
        "$@" bash "$RELEASE" ) > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

# The first line of the file matching a pattern, without a pipe into `head`.
line_of() { awk -v re="$2" '$0 ~ re { print NR; exit }' "$1"; }

# --- the manifest ------------------------------------------------------------

# python3 rather than jq, and rather than a grep for a quoted string: the suite
# already depends on python3 -- tst_Workflows.sh reads every workflow with it --
# and what these cases are asserting is that the file is a *document*, which only a
# parser can say. A grep would pass on something no application could read.
parses() {
    python3 -c 'import json, sys; json.load(open(sys.argv[1]))' "$1" 2>"$SHELLTEST_TMP/parse" \
        || {
            fail "$2 is not a JSON document"
            sed 's/^/    /' "$SHELLTEST_TMP/parse"
            sed 's/^/    /' "$1"
        }
}

# One field, or nothing at all when the file will not parse -- which `parses` is
# what reports. Never an error message where a value was expected.
manifest_field() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' \
        "$1" "$2" 2>/dev/null
}

# The changelog with every entry and marker taken out of it, committed. A cut then
# gets as far as looking for somewhere to put its marker and dies there -- which is
# the cheapest failure available *after* the manifest has been written, and that is
# what the two restore cases need.
empty_the_changelog() {
    local repo="$1"
    grep -v -e '^2026-' -e '^## ' "$repo/CHANGELOG.md" > "$repo/CHANGELOG.next"
    mv "$repo/CHANGELOG.next" "$repo/CHANGELOG.md"
    git -C "$repo" commit -q -am "Nothing written down for anything"
}

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

begin "an environment that is not configured is a refusal, not a pass"
repo=$(fixture)
touch "$repo/no-environment"
git -C "$repo" add -A && git -C "$repo" commit -q -m "No environment on this machine"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut without the live tiers running"
said "the live environment is not configured on this machine"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"

begin "a tier that ran and failed is not reported as a machine that cannot run it"
# **The fault this pair exists for.** The gate ran the tiers through make, and make
# exits 2 for any failure in a recipe -- so a heavy tier that ran for seventy
# minutes against the real machine and failed one case was reported as "the live
# environment is not configured on this machine", six lines under its own failure.
# The distinction the check draws is the useful one there is, and it failed in the
# direction that sends a reader to their own machine rather than to the fault. See
# MOLE-328.
repo=$(fixture)
touch "$repo/live-fails"
git -C "$repo" add -A && git -C "$repo" commit -q -m "The live tier will fail a case"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a failing live tier"
said "test-live is not green"
grep -qF "the live environment is not configured" "$SCRIPT_OUTPUT" \
    && fail "a tier that ran and failed was reported as an environment nobody configured"
said "aKilledUploadLeavesNothingThatLooksFinished"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"

begin "and the same for the heavy tier"
repo=$(fixture)
touch "$repo/heavy-fails"
git -C "$repo" add -A && git -C "$repo" commit -q -m "The heavy tier will fail a case"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a failing heavy tier"
said "test-heavy is not green"
grep -qF "the live environment is not configured" "$SCRIPT_OUTPUT" \
    && fail "a tier that ran and failed was reported as an environment nobody configured"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"

begin "a live suite that skipped is a refusal, and it is named"
repo=$(fixture)
touch "$repo/live-skips"
git -C "$repo" add -A && git -C "$repo" commit -q -m "The live tier will skip"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a skipped live suite"
said "tst_SftpFileSystem"
said "a suite that never met the environment is not a pass"

begin "a word in the heavy tier's report is not a skipped suite"
# The tier prints the tail of its own report, and a column in it saying
# SKIPPED-none is not a suite that skipped. An unanchored search refused this.
repo=$(fixture)
cut_release "$repo" DRY=1
[ "$SCRIPT_STATUS" = 0 ] || fail "a line in the report was read as a skipped suite"

begin "a heavy suite that skipped is a refusal even though it exits zero"
# The one that cannot be left to the tier: a QTest binary returns 0 for a case it
# skipped, so a destination with no room looks exactly like success from outside.
repo=$(fixture)
touch "$repo/heavy-skips"
git -C "$repo" add -A && git -C "$repo" commit -q -m "The heavy tier will skip"
cut_release "$repo" IGNORE=1
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a skipped heavy suite"
said "TestHeavyTransfers"
[ -z "$(git -C "$repo" tag --list)" ] || fail "a tag was made anyway"

begin "the address the tiers print does not come through this gate"
# What a release note or a ticket ends up holding. The tier says what it is talking
# to; this script is what somebody pastes.
repo=$(fixture)
cut_release "$repo" DRY=1 MOLE_TESTBED_ADDRESS=a-machine.invalid
[ "$SCRIPT_STATUS" = 0 ] || fail "the dry run failed"
grep -qF "a-machine.invalid" "$SCRIPT_OUTPUT" && fail "the address came through the gate's output"
said "<the testbed>"

begin "make's own noise is not mistaken for the version"
# This script is run by make, so every make it invokes is a recursive one -- and
# make turns on --print-directory for those. `make version | tail -1` therefore came
# back with "make[1]: Leaving directory ..." and the gate refused a release for not
# knowing what version the repository was at, after running all three tiers. Nothing
# saw it because these cases drive the script directly, where MAKEFLAGS is empty.
# MAKEFLAGS=w is that condition without needing a real recursion. See MOLE-321.
repo=$(fixture)
MAKEFLAGS=w cut_release "$repo" DRY=1
[ "$SCRIPT_STATUS" = 0 ] || fail "the gate could not read the version with make printing directories"
# The noise has to be there, or the case is not reproducing the condition; and the
# version has to have been read through it.
grep -q "Leaving directory" "$SCRIPT_OUTPUT" \
    || fail "MAKEFLAGS=w did not make the sub-make print directories, so this asserts nothing"
said "cutting 0.4.0"

begin "an override in the outer environment does not reach the script"
# The gate runs this suite before it cuts, and make exports a command-line variable
# to everything it runs -- so `make release VERSION=0.1.0` put VERSION=0.1.0 into
# the environment of every case in this file. Five of them cut that instead of what
# they asked for, and the release refused itself on its own test. The clearing in
# cut_release is what stops it; this is what stops the clearing being removed.
repo=$(fixture)
VERSION=9.9.9 MAJOR=1 MINOR=1 cut_release "$repo" DRY=1
[ "$SCRIPT_STATUS" = 0 ] || fail "the dry run failed with overrides in the environment"
grep -qF "9.9.9" "$SCRIPT_OUTPUT" && fail "a VERSION from outside reached the script"
# The fixture already has a tag, so the next cut is the patch bump -- not 1.0.0,
# which is what the MAJOR=1 in the environment would have made of it.
said "cutting 0.4.0"

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

begin "a dry run prints the manifest it would write, and writes none"
repo=$(fixture)
cut_release "$repo" DRY=1
[ "$SCRIPT_STATUS" = 0 ] || fail "a dry run failed"
said "latest.json, and what this cut writes into it:"
said '"version": "0.4.0"'
said '"format": 1'
said "releases/tag/v0.4.0"
[ ! -e "$repo/latest.json" ] || fail "a dry run wrote the manifest"
# And what it printed is a document rather than something that reads like one. A
# real cut commits exactly these bytes, so a dry run is the last chance anybody has
# to look at them.
sed -n '/^   {$/,/^   }$/p' "$SCRIPT_OUTPUT" | sed 's/^   //' > "$SHELLTEST_TMP/printed.json"
parses "$SHELLTEST_TMP/printed.json" "the manifest a dry run printed"
[ "$(manifest_field "$SHELLTEST_TMP/printed.json" version)" = "0.4.0" ] \
    || fail "the manifest a dry run printed does not name the version it would cut"

# ------------------------------------------------------------- a real cut

begin "the first cut is the version the code already claims"
repo=$(fixture)
cut_release "$repo"
[ "$SCRIPT_STATUS" = 0 ] || fail "the release did not go through"
said "the first release, which is the version the code already claims"
said "the test-live tier"
said "the test-heavy tier"
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
grep -qF "## 0.4.0 — released $TODAY" <<<"$(head -n 20 "$repo/CHANGELOG.md")" \
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
# And the manifest, which is the whole of what something running can ask. In the
# release commit rather than in a second one, and naming what was cut.
[ -f "$repo/latest.json" ] || fail "the release wrote no manifest"
case "$carried" in
    *latest.json*) ;;
    *) fail "the release commit does not carry the manifest: $carried" ;;
esac
parses "$repo/latest.json" "the manifest a real cut wrote"
[ "$(manifest_field "$repo/latest.json" version)" = "$( (cd "$repo" && make version) )" ] \
    || fail "the manifest says $(manifest_field "$repo/latest.json" version) and project(VERSION) says something else"
[ "$(manifest_field "$repo/latest.json" format)" = "1" ] \
    || fail "the manifest does not say which format it is, so nothing can tell whether it may read it"
[ "$(manifest_field "$repo/latest.json" released)" = "$TODAY" ] \
    || fail "the manifest is dated $(manifest_field "$repo/latest.json" released), and the tag was cut today"
case "$(manifest_field "$repo/latest.json" url)" in
    *"/releases/tag/v0.4.0") ;;
    *) fail "the landing page does not name the version: $(manifest_field "$repo/latest.json" url)" ;;
esac
# A trailing newline. Without one every tool that reads the file complains about
# it, and somebody eventually "fixes" it in a commit of its own.
[ -z "$(tail -c 1 "$repo/latest.json")" ] || fail "the manifest has no trailing newline"

begin "the next cut bumps the patch, and the overrides choose otherwise"
# Sequential on purpose: each cut has to put its marker above the last one, which
# is the case a fresh fixture cannot reach.
add_entry "$repo" "2026-08-24 #MOLE-119 The thing the second release carries"
cut_release "$repo"
[ "$SCRIPT_STATUS" = 0 ] || fail "the second release did not go through"
said "a patch bump, which is the default"
[ "$( (cd "$repo" && make version) )" = "0.4.1" ] || fail "the patch was not bumped in CMakeLists.txt"
[ "$(git -C "$repo" cat-file -t v0.4.1)" = tag ] || fail "v0.4.1 is not an annotated tag"

add_entry "$repo" "2026-08-25 #MOLE-120 The thing the minor release carries"
cut_release "$repo" MINOR=1
[ "$SCRIPT_STATUS" = 0 ] || fail "MINOR=1 did not go through"
[ "$( (cd "$repo" && make version) )" = "0.5.0" ] || fail "MINOR=1 did not cut 0.5.0"

add_entry "$repo" "2026-08-26 #MOLE-121 The thing the major release carries"
cut_release "$repo" MAJOR=1
[ "$SCRIPT_STATUS" = 0 ] || fail "MAJOR=1 did not go through"
[ "$( (cd "$repo" && make version) )" = "1.0.0" ] || fail "MAJOR=1 did not cut 1.0.0"

add_entry "$repo" "2026-08-27 #MOLE-122 The thing the asked-for release carries"
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

begin "the manifest names the newest cut and nothing older"
# Four more releases have gone through above. The file is the answer to one
# question -- what is the newest -- so what matters is that the last cut is what it
# says, and that it was rewritten by the release commit each time rather than
# trailing a cut behind.
[ "$(manifest_field "$repo/latest.json" version)" = "2.3.4" ] \
    || fail "the manifest says $(manifest_field "$repo/latest.json" version) after 2.3.4 was cut"
case "$(manifest_field "$repo/latest.json" url)" in
    *"/releases/tag/v2.3.4") ;;
    *) fail "the landing page still names an older release: $(manifest_field "$repo/latest.json" url)" ;;
esac
[ "$(git -C "$repo" log -1 --format=%s -- latest.json)" = "Release 2.3.4" ] \
    || fail "the manifest was last written by '$(git -C "$repo" log -1 --format=%s -- latest.json)'"

begin "a field added to the manifest does not stop the rest of it being read"
# The property `format` exists to protect: a field may be added and never renamed
# or removed, because a binary released years earlier cannot be told otherwise.
# Spliced in as text, the way a later release.sh would write one, rather than by
# re-serialising the document -- what is under test is this file's format and not
# python's json module.
sed 's/^  "format": 1,$/  "format": 1,\n  "sha256": "not a real digest",/' \
    "$repo/latest.json" > "$SHELLTEST_TMP/grown.json"
grep -q '"sha256"' "$SHELLTEST_TMP/grown.json" \
    || fail "no field was added, so this case asserts nothing"
parses "$SHELLTEST_TMP/grown.json" "a manifest with a field added to it"
for field in format version released url; do
    [ "$(manifest_field "$SHELLTEST_TMP/grown.json" "$field")" \
      = "$(manifest_field "$repo/latest.json" "$field")" ] \
        || fail "$field reads differently once a field was added beside it"
done

begin "a version that has already been cut is refused"
cut_release "$repo" VERSION=1.0.0
[ "$SCRIPT_STATUS" != 0 ] || fail "a version that is already a tag was cut again"
said "v1.0.0 is already a tag"
[ -z "$(git -C "$repo" status --porcelain)" ] || fail "it left the tree dirty"

# ------------------------------------------------- putting the manifest back

begin "a cut that stops after writing the manifest puts back what it said"
# The manifest is the first tracked file a real cut writes, so every failure after
# it has to leave the file naming the release that was actually published. 2.3.4
# was, and nobody cut 2.3.5.
before=$(cat "$repo/latest.json")
empty_the_changelog "$repo"
cut_release "$repo"
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut with nothing written down for it"
said "could not find an entry in CHANGELOG.md to put the marker above"
[ "$(cat "$repo/latest.json")" = "$before" ] \
    || fail "the manifest was left naming $(manifest_field "$repo/latest.json" version), which nobody cut"
[ -z "$(git -C "$repo" status --porcelain)" ] || {
    fail "it left the tree dirty"
    git -C "$repo" status --short | sed 's/^/    /'
}

begin "a cut that stops leaves behind no manifest the repository never had"
# The other half of that restore, and the half that would have broken everything
# else with it: on the first cut in a repository with no manifest in HEAD there is
# nothing to check one out of, and `git checkout -- a b c` is refused *whole* when
# one of its paths is unknown to git. A single checkout of every path would
# therefore have put none of them back -- not the changelog, and not the pictures.
repo=$(fixture)
empty_the_changelog "$repo"
cut_release "$repo"
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut with nothing written down for it"
[ ! -e "$repo/latest.json" ] || fail "the manifest was left behind, naming a release nobody cut"
[ "$(cat "$repo/docs/guide/images/01-shot.png")" = "a picture" ] \
    || fail "the regenerated picture was not put back, so the restore stopped at the manifest"
[ -z "$(git -C "$repo" status --porcelain)" ] || {
    fail "it left the tree dirty"
    git -C "$repo" status --short | sed 's/^/    /'
}

# ---------------------------------------------- what the remote already has
#
# Last, and with a repository of their own: the cases above build on one another
# -- a successful cut, then failures that have to put its manifest back -- and a
# fresh fixture in the middle of that sequence takes the ground out from under
# the rest.

begin "a remote that has moved is refused before anything is written"
# The gate checked the branch, the tree and the *local* tags and nothing else, so
# a remote that had moved -- a merged pull request, a commit from another clone --
# got all the way to the push: the manifest, the marker, the version line and the
# commit were written, the tag was made, the restore-on-failure was disarmed, and
# then `git push` was refused. Forty minutes of tiers, and a release commit and a
# tag to unpick by hand. ADR-0084 says a failed release leaves the manifest alone;
# that was true only of failures before the disarm. See MOLE-388.
mine=$(fixture)
before=$(git -C "$mine" rev-parse HEAD)
# Somebody else pushed. Written from a side branch of the same repository rather
# than from a second clone: what matters is that origin/main carries a commit this
# branch does not, and a clone brings its own default-branch trouble with it.
git -C "$mine" checkout -q -b theirs
echo "their work" > "$mine/theirs.txt"
git -C "$mine" add -A
git -C "$mine" commit -q -m "Somebody else got there first"
git -C "$mine" push -q origin theirs:main
git -C "$mine" checkout -q main
git -C "$mine" branch -qD theirs

cut_release "$mine"
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut on a branch behind the remote"
said "pull first"
# Nothing written, nothing committed, no tag: the refusal is in the gate.
[ -z "$(git -C "$mine" status --porcelain)" ] || {
    fail "it left the tree dirty"
    git -C "$mine" status --short | sed 's/^/    /'
}
[ "$(git -C "$mine" rev-parse HEAD)" = "$before" ] || fail "it committed something"
[ -z "$(git -C "$mine" tag --list 'v*')" ] || fail "it made a tag"
[ ! -f "$repo/latest.json" ] || fail "it wrote a manifest"

begin "a tag that exists only on the remote is refused"
# Only an exact *local* duplicate was refused, so a tag pushed from another clone
# was invisible to the gate. See MOLE-388.
mine=$(fixture)
# v0.4.1 is what the default patch bump would cut, and it is put on the remote
# and nowhere else -- the local check cannot see it.
git -C "$mine" push -q origin "HEAD:refs/tags/v0.4.1"
[ -z "$(git -C "$mine" tag --list 'v*')" ] || fail "the fixture left a local tag, so this case proves nothing"
cut_release "$mine"
[ "$SCRIPT_STATUS" != 0 ] || fail "a release was cut over a tag that is already on the remote"
said "already a tag"
[ -z "$(git -C "$mine" status --porcelain)" ] || fail "it left the tree dirty"
[ ! -f "$repo/latest.json" ] || fail "it wrote a manifest"

begin "a version below the newest one is refused"
# Only exact duplication was refused, so a VERSION= below the current version, or
# below the newest tag, was accepted -- and would then write a latest.json naming
# an older release as the newest thing there is. See MOLE-388.
mine=$(fixture)
cut_release "$mine" VERSION=0.3.0
[ "$SCRIPT_STATUS" != 0 ] || fail "a release below the version the code claims was cut"
said "is not above"
[ ! -f "$repo/latest.json" ] || fail "it wrote a manifest"

# And below the newest *tag*, which is the other half: the code can claim less
# than what has been released if somebody bumped the tag and not the file.
mine=$(fixture)
git -C "$mine" tag -a v9.9.9 -m "released long ago"
cut_release "$mine" VERSION=1.0.0
[ "$SCRIPT_STATUS" != 0 ] || fail "a release below the newest tag was cut"
said "is not above"

# A version above it is still fine, which is what says this refuses an ordering
# rather than the option.
mine=$(fixture)
cut_release "$mine" DRY=1 VERSION=0.5.0
[ "$SCRIPT_STATUS" = 0 ] || {
    fail "a version above the current one was refused"
    sed 's/^/    /' "$SCRIPT_OUTPUT"
}

done_testing
