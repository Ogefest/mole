#!/usr/bin/env bash
#
# One version's block out of the changelog: the release notes, taken rather than
# written again.
#
# The cases that matter are the two ends of the file and the two ways it can be
# empty. The oldest marker's block runs to the end -- an extractor written as
# "between two markers" finds nothing for the only marker there is, which is exactly
# the state of the first release this project will ever cut. And an empty block stops
# a release, because it means either the marker is in the wrong place or nobody wrote
# a line for anything that went in. See MOLE-123.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

BLOCK="$MOLE_SOURCE_DIR/scripts/changelog-block.sh"

# A changelog with two releases in it and prose at the end, which is the shape the
# real one will have after its second cut.
fixture() {
    cat > "$SHELLTEST_TMP/CHANGELOG.md" <<'EOF'
# Changelog

Prose about the file, which is not an entry and not a marker.

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

2026-08-25 #MOLE-9 Something landed since the last release

## 0.2.0 — released 2026-08-24

2026-08-24 #MOLE-8 The newest thing in the second release
2026-08-23 #MOLE-7 Something else in it

## 0.1.0 — released 2026-08-20

2026-08-19 #MOLE-6 The newest thing in the first release

- Prose from before the format, which belongs to the first release.
  It runs to more than one line, the way the real ones do.
EOF
    printf '%s' "$SHELLTEST_TMP/CHANGELOG.md"
}

ask() {
    bash "$BLOCK" "$1" "$2" > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

begin "a block in the middle of the file is what is between its marker and the next"
file=$(fixture)
ask 0.2.0 "$file"
exited 0
said "MOLE-8 The newest thing in the second release"
said "MOLE-7 Something else in it"
# Not the entries above its marker, which are unreleased, and not the release below.
grep -qF "MOLE-9" "$SCRIPT_OUTPUT" && fail "it took an unreleased entry into the notes"
grep -qF "MOLE-6" "$SCRIPT_OUTPUT" && fail "it took the previous release's entries"

begin "the oldest block runs to the end of the file, prose and all"
# The case the first release depends on, and the one an extractor written as
# "between two markers" gets wrong by finding nothing at all.
ask 0.1.0 "$file"
exited 0
said "MOLE-6 The newest thing in the first release"
said "Prose from before the format"
said "It runs to more than one line"

begin "a version with no marker is refused, and says which of the two it is"
ask 9.9.9 "$file"
[ "$SCRIPT_STATUS" != 0 ] || fail "a version that was never cut produced notes"
said "no release marker for 9.9.9"
said "never cut"
said "wrong shape"

begin "an empty block is refused, because it is one of two faults and both matter"
# A marker with nothing under it: either it went in above the entries it should be
# under, or nobody wrote a line for anything in the release.
cat > "$SHELLTEST_TMP/empty.md" <<'EOF'
# Changelog

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

2026-08-25 #MOLE-9 An entry that ended up above the marker

## 0.3.0 — released 2026-08-25

## 0.2.0 — released 2026-08-24

2026-08-24 #MOLE-8 The one release that has anything in it
EOF
ask 0.3.0 "$SHELLTEST_TMP/empty.md"
[ "$SCRIPT_STATUS" != 0 ] || fail "an empty block was published as a release body"
said "is empty"
said "wrong place"
said "nothing was written down"

begin "the example in the header is not mistaken for a release"
# The header states the shape by showing one. A fenced example that counted as a
# marker would cut every block short at the top of the file.
ask 0.2.0 "$file"
exited 0
grep -qF "released 2026-08-11" "$SCRIPT_OUTPUT" && fail "the header's own example was read as a marker"

begin "nothing that runs carries its own copy of the expression"
# Done when: one expression in the repository. Two exemptions, and they are stated
# rather than quietly skipped, because an exemption nobody can see is how a rule
# stops meaning anything.
#
# A **test fixture** has to state the shapes: the scripts read them out of the file
# they are given, so a fixture changelog without a header is not a changelog. And an
# **ADR** quotes the format it decided on, as of its date -- this project does not
# edit those when the world moves, so it is a record rather than a second rule.
#
# What must carry none is anything that runs: the scripts, the workflow, the
# application. Those ask the file.
: > "$SHELLTEST_TMP/copies"
grep -rlF 'released (\d{4}-\d{2}-\d{2})$' \
    "$MOLE_SOURCE_DIR/scripts" "$MOLE_SOURCE_DIR/src" "$MOLE_SOURCE_DIR/.github" \
    "$MOLE_SOURCE_DIR/Makefile" 2>/dev/null > "$SHELLTEST_TMP/copies"
if [ -s "$SHELLTEST_TMP/copies" ]; then
    fail "something that runs has its own copy of the marker expression"
    sed "s|$MOLE_SOURCE_DIR/|    |" "$SHELLTEST_TMP/copies"
fi
# And the one live copy is where everything reads it from.
grep -qF 'released (\d{4}-\d{2}-\d{2})$' "$MOLE_SOURCE_DIR/CHANGELOG.md" \
    || fail "CHANGELOG.md does not state the expression everything else asks it for"
begin "a changelog that does not state its shape produces nothing rather than guessing"
# The other half of one expression: the extractor has no copy to fall back on, so a
# file with no header states nothing and it says so. Behaviour rather than a grep for
# how it is written, which is the part that would go stale.
cat > "$SHELLTEST_TMP/silent.md" <<'EOF'
# Changelog

No fenced block, so nothing here says what a marker looks like.

## 0.2.0 — released 2026-08-24

2026-08-24 #MOLE-8 Something that landed
EOF
ask 0.2.0 "$SHELLTEST_TMP/silent.md"
[ "$SCRIPT_STATUS" != 0 ] || fail "it found a block in a file that states no expression"
said "does not state its release-marker expression"

done_testing
