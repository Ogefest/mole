#!/usr/bin/env bash
#
# What the documents promise, held against the code that has to keep it.
#
# `docs/WRITING_PLUGINS.md` is the one document somebody outside works from, and
# every name in it is a promise. Several were stale in ways neither half showed on
# its own: a sample carrying a class from a layer above the SDK, five interface
# methods the guide had never heard of, a menu section named after an enum value
# that had been split in two, and a clash check the event bus does not perform.
# The counts were worse -- four documents counted the extension points four ways,
# and two headers each called themselves "the fifth".
#
# What is cheap to hold is the part that is a *name*: an identifier in backticks
# either exists in the code or does not. The rest of a document cannot be checked
# by a script, which is what makes it worth checking the part that can.
#
# See MOLE-392.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

identifiers() {
    python3 "$MOLE_SOURCE_DIR/tests/support/read-doc-identifiers.py" "$@"
}

# The layers a plugin links, and so the only ones the plugin guide may name.
VISIBLE="src/sdk src/core"

begin "the reader can find an identifier that is not there"
# The guard on every case below: a checker that finds nothing passes everything.
# A name this project cannot possibly have, through the same path as the real
# documents.
printf 'A page mentioning `mole::NoSuchClassExistsHere` in passing.\n' \
    > "$SHELLTEST_TMP/invented.md"
if identifiers "$SHELLTEST_TMP/invented.md" $VISIBLE > "$SHELLTEST_TMP/said" 2>&1; then
    fail "an invented identifier was not reported, so the cases below prove nothing"
fi
grep -q "NoSuchClassExistsHere" "$SHELLTEST_TMP/said" \
    || fail "it reported something other than the invented name"

begin "and it can find one that exists in a layer the reader cannot see"
# The fault this is really for. The tab sample carried
# `Q_PROPERTY(mole::FileListModel* results ...)`, and FileListModel lives in
# src/ui/models -- above the SDK in the layer order, so a plugin cannot see it.
# The guide was teaching the layering being broken, and every name in it was
# spelled correctly. See MOLE-392.
{
    printf '```cpp\n'
    printf '    Q_PROPERTY(mole::FileListModel* results READ results CONSTANT)\n'
    printf '```\n'
} > "$SHELLTEST_TMP/wrong-layer.md"
if identifiers "$SHELLTEST_TMP/wrong-layer.md" $VISIBLE > "$SHELLTEST_TMP/said" 2>&1; then
    fail "a class from a layer above the SDK was accepted in a plugin sample"
fi
grep -q "FileListModel" "$SHELLTEST_TMP/said" \
    || fail "it reported something other than the class the sample cannot use"

begin "and it accepts the names a plugin author is given"
{
    printf 'Real: `mole::IPlugin`, `mole::PluginServices`, `MOLE_PLUGIN_IID`.\n'
    printf 'The harness: `mole::test::ConformanceContext`.\n'
    printf 'A case: `tst_AppIntegration::everyFeatureIsReachableFromTheMenu`.\n'
    printf 'And a sample of their own: `TestGitLabFs::conformance()`.\n'
} > "$SHELLTEST_TMP/real.md"
identifiers "$SHELLTEST_TMP/real.md" $VISIBLE > "$SHELLTEST_TMP/said" 2>&1 \
    || { fail "a document naming only real identifiers was reported as stale"
         sed 's/^/    /' "$SHELLTEST_TMP/said"; }

begin "every identifier the plugin guide names is one its reader can have"
identifiers docs/WRITING_PLUGINS.md $VISIBLE > "$SHELLTEST_TMP/stale" 2>&1 \
    || { fail "docs/WRITING_PLUGINS.md names things a plugin cannot see"
         sed 's/^/    /' "$SHELLTEST_TMP/stale"; }

begin "the plugin guide says where a plugin is built, in its first paragraph"
# A plugin cannot be built outside the tree today: mole_sdk and mole_core are
# static libraries that are not installed, and every SDK header needs core
# headers behind it. The guide showed a CMakeLists linking mole_sdk, which
# resolves only inside a directory Mole itself added -- so the first thing it
# told a reader was the one thing they could not do. See ADR-0099.
opening=$(sed -n '1,12p' docs/WRITING_PLUGINS.md)
grep -q "built inside a Mole checkout" <<< "$opening" \
    || fail "the opening does not say a plugin is built in the tree"
grep -q "0099" <<< "$opening" \
    || fail "the opening does not point at the decision that says why"

begin "the SDK is not installed, which is what the guide says"
# The other half of the same statement, read from the build rather than the
# document: if this ever stops being true, the guide is wrong and this fails
# rather than the guide going stale quietly.
grep -nE "install\(TARGETS (mole_sdk|mole_core)\b" CMakeLists.txt > "$SHELLTEST_TMP/installed"
if [ -s "$SHELLTEST_TMP/installed" ]; then
    fail "the SDK or core is installed now, so WRITING_PLUGINS.md and ADR-0099 are out of date"
    sed 's/^/    /' "$SHELLTEST_TMP/installed"
fi
grep -nE "install\(DIRECTORY src/(sdk|core)" CMakeLists.txt > "$SHELLTEST_TMP/headers"
if [ -s "$SHELLTEST_TMP/headers" ]; then
    fail "the SDK or core headers are installed now, so the documents are out of date"
    sed 's/^/    /' "$SHELLTEST_TMP/headers"
fi

begin "both hosts export their symbols, so a plugin has one core in either"
begin "every picture the guide shows exists, and every picture it keeps is shown"
# **Three pictures were in the repository and on no page**, taken by the
# walkthrough and copied in by `make guide-images`, so every regeneration put
# three binary files in front of a reviewer for nothing. The other direction is
# worse and was not checked either: a link to a picture that is not there renders
# as a broken image on GitHub. Both are one rule -- the set on disk and the set
# linked to are the same set. See MOLE-402.
: > "$SHELLTEST_TMP/pictures"
shown=$(grep -rhoE '\(images/[A-Za-z0-9._-]+\)' docs/guide/*.md | tr -d '()' | sed 's|images/||' | sort -u)
[ -n "$shown" ] || fail "no picture is linked from the guide at all, so this is not looking"

for name in $shown; do
    [ -f "docs/guide/images/$name" ] \
        || echo "the guide links to images/$name, which is not there" >> "$SHELLTEST_TMP/pictures"
done
for f in docs/guide/images/*; do
    name=$(basename "$f")
    printf '%s\n' "$shown" | grep -qx "$name" \
        || echo "images/$name is kept and shown on no page" >> "$SHELLTEST_TMP/pictures"
done

if [ -s "$SHELLTEST_TMP/pictures" ]; then
    fail "the pictures on disk and the pictures the guide shows are not the same set"
    sed 's/^/    /' "$SHELLTEST_TMP/pictures"
fi

begin "every architecture decision carries a number of its own, a date and its reasoning"
# **Two records were given 0035**, in a directory whose own README says numbers are
# never reused -- so "see ADR-0035" named two different decisions and could not be
# followed. Nine more were off the template: `Date: 2026-08-18` as a bare line
# rather than the bullet the template asks for, four with their reasoning under
# `## Alternatives`, and three with no such heading at all. None of it is a fault a
# user meets; each one sends the next reader the wrong way, which is what these
# records exist to prevent. See MOLE-402.
: > "$SHELLTEST_TMP/adrs"
count=0
for f in docs/adr/[0-9]*.md; do
    count=$((count + 1))
    name=$(basename "$f")
    number=${name%%-*}

    grep -q '^- \*\*Date:\*\* [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$' "$f" \
        || echo "$name: no dated line in the template's shape" >> "$SHELLTEST_TMP/adrs"
    grep -q '^- \*\*Status:\*\*' "$f" \
        || echo "$name: does not say whether it still stands" >> "$SHELLTEST_TMP/adrs"
    # The reasoning, under the template's heading or as a named question -- see the
    # note in docs/adr/README.md. What is refused is a record with neither.
    grep -qE '^## (Reason|Why|Whether|Alternatives)' "$f" \
        || echo "$name: names no alternative and no reason, so nobody can tell whether it still holds" \
             >> "$SHELLTEST_TMP/adrs"

    # The title says its own number, or a reference to it lands on another record.
    grep -q "^# ADR-$number: " "$f" \
        || echo "$name: its title is not ADR-$number" >> "$SHELLTEST_TMP/adrs"
done

[ "$count" -ge 90 ] || fail "found only $count decision records, which is not this directory"

# One number, one record.
twice=$(ls docs/adr/[0-9]*.md | sed 's|.*/\([0-9]*\)-.*|\1|' | sort | uniq -d)
[ -z "$twice" ] || echo "these numbers belong to more than one record: $twice" >> "$SHELLTEST_TMP/adrs"

if [ -s "$SHELLTEST_TMP/adrs" ]; then
    fail "a decision record cannot be followed or cannot be judged"
    sed 's/^/    /' "$SHELLTEST_TMP/adrs"
fi

begin "TODO.md's count of the unstable pictures is the one the check uses"
# **A number in prose beside a list in a script is a number that goes stale**, and
# this one had: TODO.md said nine, `check-screenshots.sh` named seven, and TODO's
# own paragraph then walked through seven. Whoever reads the note next has to be
# able to trust it, so the two are held together here rather than by somebody
# remembering to edit both. See MOLE-402.
expected=$(sed -n "s/^EXPECTED='\(.*\)'$/\1/p" scripts/check-screenshots.sh)
[ -n "$expected" ] || fail "cannot find the list of unstable pictures in check-screenshots.sh"
named=$(printf '%s' "$expected" | tr ',' '\n' | grep -c .)

# The word, because the sentence is prose. Written out to twelve, which is well
# past the number anybody should be willing to live with.
words=(zero One Two Three Four Five Six Seven Eight Nine Ten Eleven Twelve)
word="${words[$named]:-}"
[ -n "$word" ] || fail "$named pictures cannot be photographed twice, which is too many to write out"

said=$(sed -n "s/^- \*\*\([A-Za-z]*\) of the guide's pictures cannot be identical twice running.*/\1/p" TODO.md)
[ -n "$said" ] || fail "TODO.md no longer says how many pictures cannot be photographed twice"
[ "$said" = "$word" ] || fail "TODO.md says $said and check-screenshots.sh names $named ($word)"

# A plugin statically links mole_core, so it carries its own copy. The host's
# exported symbols are what make the dynamic linker resolve the plugin's
# references to the host's copy -- one core in the process rather than two, with
# one set of function-local statics. `mole` had it for backtraces and
# `mole-tasks` had nothing, so the same plugin binary was in two linking regimes
# depending on which host loaded it. See ADR-0099.
for target in mole mole-tasks; do
    grep -rqE "set_target_properties\($target PROPERTIES ENABLE_EXPORTS ON\)" src \
        || fail "$target does not export its symbols, so a plugin loaded there runs its own core"
done

done_testing
