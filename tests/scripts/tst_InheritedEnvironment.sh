#!/usr/bin/env bash
#
# Every shell test in this directory, run again with a documented command's
# variables lying about in the environment, and required to answer the same.
#
# `make release VERSION=0.1.0` could not pass its own gate. The gate runs the fast
# tier before it cuts anything, and **make exports a command-line variable to
# everything it runs** -- so `VERSION=0.1.0` reached tst_Release.sh, which drives
# `scripts/release.sh` against a fake repository of its own. Five of its cases
# stopped asserting what they were written to assert and cut 0.1.0 instead, the
# suite went red, and the gate refused the release on its own test. Found on
# 2026-09-01 by the first real use of `make release`; nothing had seen it because
# the tests that drive the release script had only ever been run without an
# override. See MOLE-319.
#
# **The fix that landed was in one file and the hole is general.** Most tests here
# invoke a script from `scripts/`, and any of them that reads a variable the caller
# might also have set has the same shape. Rather than reviewing six files and
# hoping about the seventh, this asserts the property itself: a shell test's answer
# does not depend on the environment it was started in. A test that cannot be
# trusted about that cannot be trusted about anything.
#
# **Hostile here does not mean "everything".** It means the variables a documented
# command exports: `make release VERSION=x`, `make PRESET=release`,
# `make install PREFIX=~/.local`. Those reach every test in the suite through make
# itself, which is exactly how this happened. `MAKE` is deliberately *not* in the
# list -- the Makefile hands it to `release.sh` on purpose, so a test that broke
# without it would be reporting a real dependency rather than a fault.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

# Values chosen so that a script reading one could not answer the same by
# coincidence: not the version this repository is at, not the branch releases are
# cut from, not the remote they are pushed to.
hostile=(
    VERSION=9.9.9 MAJOR=1 MINOR=1 DRY=1 BRANCH=not-main REMOTE=not-origin
    PRESET=release JOBS=1 PREFIX=/nowhere DESTDIR=/nowhere
    MOLE_LOG=all
)

begin "there are shell tests to run again at all"
# The guard every case here rests on: a find that matched nothing would pass
# everything below it.
mine=$(basename "${BASH_SOURCE[0]}")
tests=$(find tests/scripts -name 'tst_*.sh' ! -name "$mine" | sort)
count=$(printf '%s\n' "$tests" | grep -c .)
[ "$count" -ge 10 ] || fail "found only $count other shell tests; expected the whole tier"

begin "every shell test answers the same with a documented command's variables set"
for one in $tests; do
    if env "${hostile[@]}" bash "$one" > "$SHELLTEST_TMP/rerun" 2>&1; then
        pass_note "$(basename "$one") ignores what it inherited"
        continue
    fi
    fail "$(basename "$one") answers differently when it inherits an environment"
    # The tail rather than the whole run: the inner file has already said which of
    # its own cases failed, and its own output is what explains it.
    sed 's/^/    /' "$SHELLTEST_TMP/rerun" | tail -25
done

done_testing
