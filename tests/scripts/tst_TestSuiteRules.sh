#!/usr/bin/env bash
#
# What every C++ test suite must be true of, checked by reading it.
#
# Companion to tst_ShellScripts.sh, which does the same for `scripts/`. The point
# of a static rule is that a new suite joins it by existing rather than by
# somebody remembering, which is the difference between a rule and a habit.
#
# Two rules live here, and what they have in common is the tier they keep alive:
# both faults made `make tsan` red for everybody, and neither showed up as a
# failing assertion in the suite that caused it.
#
# The first is MOLE-273. A task logs "started" from a pool thread; in a test
# binary the Qt message handler *is* testlib's logger, which reads the global
# naming the test function currently running; and the main thread writes that
# same global on its way into the next one. So a suite that lets a test function
# end with a task still running races the harness itself. Nothing is corrupted --
# every assertion passes -- and ThreadSanitizer fails the run every time, which
# made `make tsan` permanently red and would have buried a real race in a tier
# nobody trusted.
#
# TaskManager's destructor cancels and joins the pool, so a suite that destroys
# its manager in cleanup() cannot be in that state. It has to be cleanup() and
# not the next test function's setup: by then the harness has already moved on,
# which is exactly the shape the fault had in tst_TaskListModel, where `build()`
# reassigned the manager at the top of the following case.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

owners=$(grep -rl 'unique_ptr<TaskManager>' tests --include='*.cpp' | sort)

begin "there are suites owning a TaskManager to check at all"
# A match that found nothing would make the case below vacuously green, which is
# the failure mode of every test that iterates over a glob.
count=$(printf '%s\n' "$owners" | grep -c .)
[ "$count" -ge 20 ] || fail "found only $count suites owning a TaskManager; expected the task tier"

begin "a suite that owns a TaskManager destroys it in cleanup()"
: > "$SHELLTEST_TMP/undrained"
for f in $owners; do
    member=$(sed -n 's/.*unique_ptr<TaskManager>[ \t]*\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$f" | head -1)
    if [ -z "$member" ]; then
        echo "$f: cannot tell what the TaskManager member is called" >> "$SHELLTEST_TMP/undrained"
        continue
    fi

    awk -f tests/support/cleanup-body.awk "$f" > "$SHELLTEST_TMP/cleanup"
    if [ ! -s "$SHELLTEST_TMP/cleanup" ]; then
        echo "$f: owns $member and has no cleanup() at all" >> "$SHELLTEST_TMP/undrained"
        continue
    fi
    grep -q "${member}\.reset()" "$SHELLTEST_TMP/cleanup" \
        || echo "$f: cleanup() does not destroy $member" >> "$SHELLTEST_TMP/undrained"
done

if [ -s "$SHELLTEST_TMP/undrained" ]; then
    fail "a task can outlive its test function and race the harness -- see MOLE-273"
    sed 's/^/    /' "$SHELLTEST_TMP/undrained"
fi

# The second rule is MOLE-292, and it is about a configuration rather than a run.
#
# The tsan tier builds against an instrumented Qt that is qtbase only -- ADR-0055
# says why building more of it that way is not a thing to ask of anybody -- so the
# preset sets MOLE_CORE_ONLY, the top-level CMakeLists leaves out src/plugins and
# src/app, and this file leaves out every suite that needs them. A suite added
# above that guard which links something a qtbase-only build has not got does not
# fail its own case: it fails `cmake` at generate time, for the whole tier, with an
# error naming a test that has nothing to do with whatever was being changed. That
# is how the tier sat unusable from the day tst_Palette was added until somebody
# touched a CMakeLists and forced a reconfigure.
#
# An allowlist rather than a list of what is forbidden, and both halves of it are
# read from the build files: a Qt component nobody has heard of, or a library from
# a subdirectory that is not built, is refused by existing rather than by being
# remembered.

begin "the suites built in a core-only configuration link only what one has"

# The Qt components a qtbase-only build has, taken from the branch that asks for
# them, plus the two the test tier always finds for itself.
allowedQt=$(sed -n '/^if(MOLE_CORE_ONLY)/,/^else()/p' CMakeLists.txt \
            | sed -n 's/.*COMPONENTS \(.*\))/\1/p') || true
allowedQt="$allowedQt Test Network"
[ -n "$(echo "$allowedQt" | tr -d ' ')" ] || fail "cannot tell which Qt components a core-only build asks for"

# The libraries the always-built subdirectories define. src/plugins and src/app are
# the two the top-level guard leaves out, so nothing they define may appear here.
allowedLibs="mole_flags mole_test_support"
for dir in core sdk host ui tools; do
    allowedLibs="$allowedLibs $(sed -n 's/^ *\(qt_\)\?add_library(\([A-Za-z0-9_]*\).*/\2/p' \
                                "src/$dir/CMakeLists.txt" | tr '\n' ' ')"
done
case " $allowedLibs " in
    *" mole_core "*|*" mole_ui "*) ;;
    *) fail "cannot tell which libraries the always-built subdirectories define" ;;
esac

# Every mole_add_test call outside an `if(NOT MOLE_CORE_ONLY)` region, one per line,
# whether or not it was written on one. Parentheses are counted rather than assumed:
# the longer calls in this file wrap.
awk '
    /^[ \t]*if\(NOT MOLE_CORE_ONLY\)/ { depth++; if (!guard) guard = depth; next }
    /^[ \t]*if\(/  { depth++; next }
    /^[ \t]*endif/ { if (guard && depth == guard) guard = 0; depth--; next }
    /mole_add_test\(/ { collecting = 1; text = "" }
    collecting {
        text = text " " $0
        opens = gsub(/\(/, "(")
        closes = gsub(/\)/, ")")
        balance += opens - closes
        if (balance <= 0) { collecting = 0; balance = 0; if (!guard) print text }
    }
' tests/CMakeLists.txt > "$SHELLTEST_TMP/unguarded"

count=$(grep -c . "$SHELLTEST_TMP/unguarded")
[ "$count" -ge 30 ] || fail "found only $count suites above the guard; expected the core and ui tiers"

: > "$SHELLTEST_TMP/unbuildable"
while read -r call; do
    name=$(echo "$call" | sed -n 's/.*mole_add_test( *\([A-Za-z0-9_]*\).*/\1/p')
    libs=$(echo "$call" | sed -n 's/.*LIBS *\(.*\))/\1/p')
    for lib in $libs; do
        case "$lib" in
            Qt6::*)
                component=${lib#Qt6::}
                case " $allowedQt " in
                    *" $component "*) ;;
                    *) echo "$name links $lib, which a qtbase-only build has not got" \
                       >> "$SHELLTEST_TMP/unbuildable" ;;
                esac
                ;;
            mole*)
                case " $allowedLibs " in
                    *" $lib "*) ;;
                    *) echo "$name links $lib, which a core-only build does not define" \
                       >> "$SHELLTEST_TMP/unbuildable" ;;
                esac
                ;;
        esac
    done
done < "$SHELLTEST_TMP/unguarded"

if [ -s "$SHELLTEST_TMP/unbuildable" ]; then
    fail "cmake cannot generate a core-only build, so the whole tsan tier is red -- see MOLE-292"
    sed 's/^/    /' "$SHELLTEST_TMP/unbuildable"
fi

begin "every directory that reads a plugin's availability is added after src/plugins"
# **A cache variable one directory writes and another reads is order-dependent,
# and CMake will not say so.** src/plugins writes MOLE_HAVE_ARCHIVE_PLUGIN and
# MOLE_HAVE_NETWORK_PLUGIN with `set(... CACHE INTERNAL "")`, because
# find_package results do not cross between sibling directories. A directory
# added *above* it therefore reads nothing on a fresh configure and the cached ON
# on every configure after that.
#
# src/tools was above it, so `mole-tasks compress` said "this build has no
# archive support (libarchive was not found)" in every published artefact -- every
# CI job, every packaging container, `make deb` -- and worked in a developer's
# frequently reconfigured tree. The suite was green either way, because tests/ is
# configured after src/plugins. See MOLE-386.
readers=$(grep -rlE 'MOLE_HAVE_(ARCHIVE|NETWORK)_PLUGIN' --include=CMakeLists.txt src \
          | sed 's|/CMakeLists.txt$||' | sed 's|^src/||' | sort -u)
[ -n "$readers" ] || fail "no directory reads a plugin's availability -- has the variable been renamed?"

pluginsAt=$(grep -n '^ *add_subdirectory(src/plugins)' CMakeLists.txt | head -1 | cut -d: -f1)
[ -n "$pluginsAt" ] || fail "cannot find where the root file adds src/plugins"

for reader in $readers; do
    # src/plugins reads its own variables, which is where they are written.
    [ "$reader" = "plugins" ] && continue
    at=$(grep -n "^ *add_subdirectory(src/$reader)" CMakeLists.txt | head -1 | cut -d: -f1)
    if [ -z "$at" ]; then
        # Added from somewhere else, or not at all: worth saying rather than
        # passing over.
        fail "src/$reader reads a plugin's availability and the root file does not add it"
        continue
    fi
    if [ "$at" -lt "$pluginsAt" ]; then
        fail "src/$reader is added at line $at, above src/plugins at line $pluginsAt, so it reads MOLE_HAVE_*_PLUGIN before anything sets it"
    else
        pass_note "src/$reader is added after src/plugins"
    fi
done

begin "no suite includes a POSIX header outside a guard"
# A suite that cannot be *built* stops a whole tier, where a case that cannot run
# costs one line of output -- and the difference is what a Windows build turns on.
# Two files had to be changed by hand for MOLE-124: one read /proc and called
# sysconf, and one crashes a forked child in every case and is registered on Unix
# only now. This is what stops the third being found by a red build on a machine
# nobody here has.
#
# The include has to sit inside a `#if defined(Q_OS_UNIX)` or the like -- or the
# whole suite has to be registered inside `if(UNIX)` in tests/CMakeLists.txt, which
# is the right answer when every case in it is Unix's and guarding the includes
# would leave a suite with nothing in it. Both are read for here, so the rule
# accepts the resolution that fits rather than forcing one shape.
unix_only=$(awk '
    /^[[:space:]]*if[[:space:]]*\(UNIX\)/ { inside = 1; next }
    /^[[:space:]]*endif/ { inside = 0; next }
    inside && /mole_add_test\(/ {
        if (match($0, /mole_add_test\([[:space:]]*[A-Za-z0-9_]+/)) {
            name = substr($0, RSTART, RLENGTH)
            sub(/mole_add_test\([[:space:]]*/, "", name)
            print name
        }
    }
' "$MOLE_SOURCE_DIR/tests/CMakeLists.txt")

: > "$SHELLTEST_TMP/unguarded"
find "$MOLE_SOURCE_DIR/tests" -name '*.cpp' -o -name '*.h' | sort | while IFS= read -r file; do
    awk -v name="$file" '
        /^[[:space:]]*#[[:space:]]*if/ {
            depth++
            if ($0 ~ /Q_OS_UNIX|Q_OS_LINUX|Q_OS_MACOS|__unix__|__linux__|__APPLE__/) guard[depth] = 1
            next
        }
        /^[[:space:]]*#[[:space:]]*endif/ { delete guard[depth]; depth--; next }
        /^[[:space:]]*#[[:space:]]*include[[:space:]]*<(unistd|pty|termios|dlfcn|pwd|poll|fcntl)[.]h>/ ||
        /^[[:space:]]*#[[:space:]]*include[[:space:]]*<sys\// {
            inside = 0
            for (d in guard) inside = 1
            if (!inside) printf "%s:%d:%s\n", name, FNR, $0
        }
    ' "$file" > "$SHELLTEST_TMP/hits"
    [ -s "$SHELLTEST_TMP/hits" ] || continue
    suite=$(basename "$file" .cpp)
    suite=$(basename "$suite" .h)
    case " $unix_only " in
        *" $suite "*) continue ;;   # built on Unix only, so no other compiler sees it
    esac
    cat "$SHELLTEST_TMP/hits" >> "$SHELLTEST_TMP/unguarded"
done
if [ -s "$SHELLTEST_TMP/unguarded" ]; then
    fail "a suite includes a POSIX header with nothing to switch it off, so a build on another platform fails to compile rather than skipping a case"
    sed "s|$MOLE_SOURCE_DIR/||" "$SHELLTEST_TMP/unguarded" | sed 's/^/    /'
fi

begin "a shell test answers the same whether make or ctest started it"
# `make test` exports MAKEFLAGS and MAKELEVEL to everything below it, so a `make`
# run by a test was a recursive one: it prints "Entering directory" and "Leaving
# directory" on stdout, and a test reading what make said got a directory line.
# tst_Version and tst_Release both failed under `make test` and passed under a
# bare `ctest` because of it -- an answer that depends on how the suite was
# started is worse than either answer. The harness clears them; this is the check
# that it still does. See MOLE-297.
cat > "$SHELLTEST_TMP/probe.sh" <<'PROBE'
. "$MOLE_SUPPORT_DIR/shelltest.sh"
echo "level=[${MAKELEVEL:-}] flags=[${MAKEFLAGS:-}]"
exit 0
PROBE
MOLE_SUPPORT_DIR="$MOLE_SOURCE_DIR/tests/support" MAKELEVEL=3 MAKEFLAGS=" -j9 --no-print-directory" \
    bash "$SHELLTEST_TMP/probe.sh" > "$SHELLTEST_TMP/seen" 2>&1
grep -q "level=\[\] flags=\[\]" "$SHELLTEST_TMP/seen" \
    || { fail "a script under test sees make's own environment"; sed 's/^/    /' "$SHELLTEST_TMP/seen"; }

# And the thing it protects, asked of the real target: what `make version` prints
# is the version, whoever is calling it.
said=$(make version 2>/dev/null | tail -1)
[ "$said" = "$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt)" ] \
    || fail "make version said '$said', which is not the version"

done_testing
