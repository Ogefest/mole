#!/usr/bin/env bash
#
# What every C++ test suite must be true of, checked by reading it.
#
# Companion to tst_ShellScripts.sh, which does the same for `scripts/`. The point
# of a static rule is that a new suite joins it by existing rather than by
# somebody remembering, which is the difference between a rule and a habit.
#
# The rule here is MOLE-273. A task logs "started" from a pool thread; in a test
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

done_testing
