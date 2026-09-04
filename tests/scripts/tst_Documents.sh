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
