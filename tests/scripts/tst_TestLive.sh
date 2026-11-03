#!/usr/bin/env bash
#
# The live tier, held offline: what it does with a suite that never finishes.
#
# On 2026-09-05 the release gate's live tier ran for twenty-five minutes against a
# suite that was never going to finish, and nothing stopped it or said what it was
# waiting on. The hang itself was MOLE-418's data race; this is about the next one,
# which will have a different cause. So the tier gives every case a deadline, says
# which case it is in while a case is slow, and reports a case cut short as a
# timeout naming it -- not as a failed assertion and not as an environment nobody
# configured. None of that needs a server, so none of this does: the suites here
# are scripts that print what a QtTest binary prints. See MOLE-424.

. "$(dirname "$0")/../support/shelltest.sh"

LIVE="$MOLE_SOURCE_DIR/scripts/testbed/test-live.sh"

# A build directory holding the six live suites, every one of which passes --
# except the ones a case names, which do what the case asks of them.
fake_build() {
    local build="$SHELLTEST_TMP/build"
    rm -rf "$build" && mkdir -p "$build/tests"
    local suite
    for suite in tst_SftpFileSystem tst_WebdavFileSystem tst_FtpFileSystem tst_S3FileSystem \
                 tst_SmbFileSystem tst_NfsFileSystem; do
        local class="Test${suite#tst_}"
        printf '%s\n' '#!/usr/bin/env bash' \
            "echo 'INFO   : $class::initTestCase() entering'" \
            "echo 'PASS   : $class::initTestCase()'" \
            "echo 'INFO   : $class::aFileComesBack() entering'" \
            "echo 'PASS   : $class::aFileComesBack()'" \
            "echo 'Totals: 2 passed, 0 failed, 0 skipped, 0 blacklisted, 12ms'" \
            > "$build/tests/$suite"
        chmod +x "$build/tests/$suite"
    done
    printf '%s' "$build"
}

# A suite that enters a case and never leaves it. The marker is how the case
# finds out afterwards whether anything it started is still running.
hangs_in() {
    local build="$1" suite="$2" fn="$3"
    local class="Test${suite#tst_}"
    printf '%s\n' '#!/usr/bin/env bash' \
        "echo 'INFO   : $class::initTestCase() entering'" \
        "echo 'PASS   : $class::initTestCase()'" \
        "echo 'INFO   : $class::$fn() entering'" \
        "exec -a mole-live-hang-$$ sleep 600" \
        > "$build/tests/$suite"
}

# The tier, configured enough to run and pointed nowhere real. MOLE_TEST_CONTROL
# is a command that does nothing, so the tier neither looks for a control channel
# over ssh nor sweeps anybody's leftovers. Bounded from outside as well: a tier
# with no deadline would hold this case for ten minutes, and `timeout` turns that
# into an answer.
run_live() {
    local build="$1"
    shift
    env MOLE_TESTBED_ADDRESS=a-machine.invalid MOLE_TESTBED_PASSWORD=not-a-password \
        MOLE_TEST_CONTROL=: "$@" timeout 120 bash "$LIVE" "$build" > "$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

begin "a case that never finishes is cut off at the deadline, by name"
build=$(fake_build)
hangs_in "$build" tst_FtpFileSystem aListingThatNeverEnds
started=$SECONDS
run_live "$build" MOLE_LIVE_CASE_DEADLINE=3 MOLE_LIVE_HEARTBEAT=1
[ "$SCRIPT_STATUS" != 124 ] || fail "the tier was still running two minutes later"
[ $((SECONDS - started)) -lt 60 ] || fail "the tier took $((SECONDS - started)) s to give up on a 3 s deadline"
said "TIMEOUT"
said "TestFtpFileSystem::aListingThatNeverEnds()"
said "did not finish within 3 s"
# Not green, and not the code that means "this machine is not configured":
# that one sends the reader to their own machine instead of to the fault.
[ "$SCRIPT_STATUS" != 0 ] || fail "a tier with a case cut off came back green"
[ "$SCRIPT_STATUS" != 2 ] || fail "a timeout was reported as an environment nobody configured"
# The suites after it still ran: the deadline stops a case, not the tier.
said "1 failed"
uncoloured=$(sed 's/\x1b\[[0-9;]*m//g' "$SCRIPT_OUTPUT")
grep -qE 'ok +tst_S3FileSystem' <<<"$uncoloured" || fail "the suites after the stuck one did not run"
# And nothing it started is left behind holding a connection.
! pgrep -f "mole-live-hang-$$" >/dev/null || {
    fail "the stuck suite is still running after the tier gave up on it"
    pkill -f "mole-live-hang-$$"
}
cp "$SCRIPT_OUTPUT" "$SHELLTEST_TMP/hung"

begin "a slow case says what it is while it is slow"
cp "$SHELLTEST_TMP/hung" "$SCRIPT_OUTPUT"
# Seen from outside before the deadline arrives, which is what makes a slow
# case and a stuck one look different to whoever is watching.
said "still in TestFtpFileSystem::aListingThatNeverEnds()"

begin "a timeout is not mistaken for an ordinary failed assertion"
cp "$SHELLTEST_TMP/hung" "$SCRIPT_OUTPUT"
grep -q "^ *FAIL " "$SCRIPT_OUTPUT" && fail "the timeout was reported as a FAIL"
# Nor for a skip, which the release gate refuses for a different reason.
grep -qE '^[[:space:]]*SKIP' "$SCRIPT_OUTPUT" && fail "the timeout was reported as a SKIP"

begin "a suite that finishes inside the deadline is untouched"
build=$(fake_build)
run_live "$build" MOLE_LIVE_CASE_DEADLINE=30
exited 0
said "6 ran, 0 skipped, 0 failed"
grep -q "TIMEOUT\|still in" "$SCRIPT_OUTPUT" && fail "a quick tier said something about a deadline"

done_testing
