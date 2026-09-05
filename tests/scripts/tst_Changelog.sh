#!/usr/bin/env bash
#
# The changelog is a structured log, and this holds it to being one.
#
# CHANGELOG.md is the release notes: a version's notes are the block between two
# release markers, pulled out by a regular expression, so a line in any other
# shape is a line nobody will ever read. A format nothing checks is a format that
# drifts, and this one has a pipeline reading it. See MOLE-116 and ADR-0080.
#
# The two expressions are not written here. They are read out of the file's own
# header, which is where a person looking for them will look, so the file and the
# thing that checks it cannot come apart -- and the case below proves the
# expressions actually work before anything is asserted with them, because a
# pattern that matched nothing would make "nothing else matches" green by
# accident.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

CHANGELOG=CHANGELOG.md

# The header writes \d, which is how the expression reads everywhere else it is
# quoted; grep -E wants [0-9]. The only translation, and it is here in the open.
ere() { printf '%s' "${1//\\d/[0-9]}"; }

ENTRY_RE=""
MARKER_RE=""

# The file with the inside of every fenced block blanked out, so a whole-file grep
# cannot see the header's own examples -- it shows one of each shape, and an example
# must not be counted as content. Blanked rather than deleted, so a line number
# still means what it says.
outside_fences() {
    awk '/^```/ { fence = !fence; print ""; next } { print fence ? "" : $0 }' "$1"
}

# Reads the two shapes out of a file's header. They live in a fenced block, one
# per line, labelled -- prose is for people and a label is for both.
read_shapes() {
    local fence=0 line
    ENTRY_RE=""
    MARKER_RE=""
    while IFS= read -r line; do
        case "$line" in '```'*) fence=$((1 - fence)); continue ;; esac
        [ "$fence" = 1 ] || continue
        case "$line" in
            entry:*) ENTRY_RE=$(ere "$(printf '%s' "${line#entry:}" | sed 's/^ *//')") ;;
            marker:*) MARKER_RE=$(ere "$(printf '%s' "${line#marker:}" | sed 's/^ *//')") ;;
        esac
    done < "$1"
}

# Everything the file is not allowed to be, one complaint per line.
#
# Three regions and no headings to divide them, which is deliberate: **every `##`
# line in this file is a release marker**, so there is one rule to enforce rather
# than a list of permitted exceptions, and the heading somebody adds in a year
# cannot silently become a release.
#
# The header is the prose at the top and it ends at the first line that is an entry
# or a marker. The log runs from there. The prose block from before the format is
# the tail of the file -- bullets, and continuations of bullets -- and once it has
# started nothing dated may follow, which is what stops a mistyped entry from
# quietly ending the checked region. Fenced blocks are skipped throughout: the
# header shows an example of each shape, and an example must not read as content,
# which is why the header states the rule as "outside a fenced code block".
check_file() {
    local file="$1" fence=0 region=header line n=0 lastDate="" date isEntry isMarker
    while IFS= read -r line; do
        n=$((n + 1))
        case "$line" in '```'*) fence=$((1 - fence)); continue ;; esac
        [ "$fence" = 0 ] || continue
        [ -n "$line" ] || continue

        isEntry=0
        isMarker=0
        grep -qE "$ENTRY_RE" <<<"$line" && isEntry=1
        grep -qE "$MARKER_RE" <<<"$line" && isMarker=1

        # The one rule, and it is checked wherever the line is: a second-level
        # heading that is not a release marker is a release waiting to happen.
        case "$line" in
            '## '*)
                [ "$isMarker" = 1 ] || echo "$file:$n: a ## heading that is not a release marker: $line"
                ;;
        esac

        if [ "$region" = header ]; then
            # Still the prose that explains the file, unless the log starts here.
            if [ "$isEntry" = 1 ] || [ "$isMarker" = 1 ]; then
                region=log
            else
                continue
            fi
        fi

        if [ "$region" = prose ]; then
            [ "$isEntry" = 1 ] && echo "$file:$n: an entry below the prose block: $line"
            [ "$isMarker" = 1 ] && echo "$file:$n: a release marker below the prose block: $line"
            continue
        fi

        if [ "$isEntry" = 0 ] && [ "$isMarker" = 0 ]; then
            # A bullet, or the continuation of one, is where the old prose starts.
            case "$line" in
                '- '* | ' '*)
                    region=prose
                    continue
                    ;;
            esac
            echo "$file:$n: neither an entry nor a release marker: $line"
            continue
        fi

        # A marker's date is its release day and an entry's is the day it landed,
        # and in a newest-first file both run one way. Checked together for that
        # reason: an entry under a marker is older than the release, and the
        # unreleased ones above it are newer.
        if [ "$isEntry" = 1 ]; then
            date=${line%% *}
            case "$line" in
                *'#'[0-9]*) echo "$file:$n: a bare issue number is a dead link -- write #MOLE-nn: $line" ;;
            esac
        else
            date=${line##* }
        fi
        if [ -n "$lastDate" ] && [[ "$date" > "$lastDate" ]]; then
            echo "$file:$n: $date is above $lastDate, and this file is newest first"
        fi
        lastDate=$date
    done < "$file"
}

# --- the expressions themselves ---------------------------------------------

begin "the file writes down what shape its own lines are in"
read_shapes "$CHANGELOG"
[ -n "$ENTRY_RE" ] || fail "the header does not write out the entry expression"
[ -n "$MARKER_RE" ] || fail "the header does not write out the release marker expression"

# Both ways round, because an expression that matches nothing would make every
# case below pass by matching nothing either.
grep -qE "$ENTRY_RE" <<<'2026-08-11 #MOLE-112 A phrase' \
    || fail "the entry expression does not match an entry"
grep -qE "$ENTRY_RE" <<<'2026-08-11 MOLE-112 no hash at all' \
    && fail "the entry expression matches a line that is not one"
grep -qE "$MARKER_RE" <<<'## 0.2.0 — released 2026-08-11' \
    || fail "the marker expression does not match a marker"
grep -qE "$MARKER_RE" <<<'## Unreleased' \
    && fail "the marker expression matches an ordinary heading"

# --- the file --------------------------------------------------------------

PLAIN="$SHELLTEST_TMP/plain"
outside_fences "$CHANGELOG" > "$PLAIN"

begin "every line of the log is an entry or a release marker"
check_file "$CHANGELOG" > "$SHELLTEST_TMP/complaints"
if [ -s "$SHELLTEST_TMP/complaints" ]; then
    fail "a line the release notes cannot use -- see the header of $CHANGELOG"
    sed 's/^/    /' "$SHELLTEST_TMP/complaints"
fi

entries=$(grep -cE "$ENTRY_RE" "$PLAIN")
[ "$entries" -ge 100 ] || fail "only $entries entries found; the parse has stopped working"

begin "the prose from before the format is the tail of the file and nothing dated is below it"
prose=$(grep -c '^- ' "$PLAIN")
[ "$prose" -ge 50 ] || fail "only $prose prose bullets found; expected the first release's"

# The other half is in the run above -- an entry or a marker below the bullets is a
# complaint -- and this holds the shape that rule depends on: the bullets are one
# block at the end rather than scattered through the log.
last_entry=$(grep -nE "$ENTRY_RE" "$PLAIN" | tail -1 | cut -d: -f1)
first_bullet=$(grep -n '^- ' "$PLAIN" | head -1 | cut -d: -f1)
[ -n "$last_entry" ] && [ -n "$first_bullet" ] \
    || fail "cannot find the boundary between the log and the prose"
[ "$first_bullet" -gt "$last_entry" ] \
    || fail "a prose bullet at line $first_bullet is above the last entry at line $last_entry"

begin "the release markers are newest first, each version once, and are the only ## lines"
# Not "there are none", which is what this case said until MOLE-118 built the thing
# that writes one: a case asserting the absence would have failed the release that
# ended it, which is the opposite of what a check is for. The file holds none today
# and that is printed rather than asserted.
#
# What survives a cut is the order. A version's notes are the block below its
# marker, so a marker out of place, or a version marked twice, hands a reader
# another release's changes under this one's number.
markers=$(grep -E "$MARKER_RE" "$PLAIN" | sed 's/^## \([0-9.]*\) .*/\1/')
count=$(printf '%s\n' "$markers" | grep -c .)
echo "  the file holds $count release markers"
[ "$count" = "$(printf '%s\n' "$markers" | sort -u | grep -c .)" ] || fail "a version is marked more than once"
[ "$markers" = "$(printf '%s\n' "$markers" | sort -rV)" ] \
    || fail "the markers are not newest first: $(printf '%s ' $markers)"
# And the rule the header states, over the whole file: every ## line is one of them.
[ "$count" = "$(grep -c '^## ' "$PLAIN")" ] || fail "a ## line in the file is not a release marker"

# --- what it catches -------------------------------------------------------
#
# On fixtures, because the shapes have to be held before the thing that produces
# them exists: there is no release marker in the real file yet -- `make release`
# writes the first one (MOLE-118) -- and a check that could only be exercised
# after the first release would be a check nobody trusted on the day it mattered.

begin "a file in the shape the header describes is accepted"
cat > "$SHELLTEST_TMP/good.md" <<'EOF'
# Changelog

Prose about the file, which is not an entry and not a marker.

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

2026-08-23 #MOLE-291 A phrase about a change

## 0.2.0 — released 2026-08-11

2026-08-11 #MOLE-112 An older phrase
2026-08-10 #MOLE-97 An older one still

- Prose from before the format, which says #MOLE-nothing and 1.2.3 and released.
  It runs to more than one line, the way the real ones do.
EOF
check_file "$SHELLTEST_TMP/good.md" > "$SHELLTEST_TMP/complaints"
if [ -s "$SHELLTEST_TMP/complaints" ]; then
    fail "a file in the right shape was complained about"
    sed 's/^/    /' "$SHELLTEST_TMP/complaints"
fi

begin "the five ways to break it are each caught"
cat > "$SHELLTEST_TMP/bad.md" <<'EOF'
# Changelog

Prose about the file.

2026-08-23 #MOLE-1 A phrase

## Unreleased

2026-08-24 #MOLE-2 Dated after the one above it
2026-08-20 #MOLE-3 A phrase that says fixes #22 in it

- Prose from before the format.

2026-08-19 #MOLE-4 An entry below the prose block
EOF
check_file "$SHELLTEST_TMP/bad.md" > "$SHELLTEST_TMP/complaints"
for expected in \
    "neither an entry nor a release marker: ## Unreleased" \
    "is above 2026-08-23, and this file is newest first" \
    "a bare issue number is a dead link" \
    "a ## heading that is not a release marker: ## Unreleased" \
    "an entry below the prose block"; do
    grep -qF "$expected" "$SHELLTEST_TMP/complaints" \
        || fail "nothing said: $expected"
done

done_testing
