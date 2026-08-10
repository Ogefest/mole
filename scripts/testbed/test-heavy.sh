#!/usr/bin/env bash
#
# The heavy tiers: gigabytes in both directions, and then the same transfers
# with the machine being attacked while they run.
#
# Separate from `make test` because it moves real data and takes real minutes,
# and separate from `make test-live` because that one is about whether the
# backends are correct while this one is about what happens at a size. What it
# asserts beyond "the copy worked" is the part that matters: peak temporary
# space, resident memory, file descriptors, and throughput recorded run over run.
#
# **It asks the machine how much room it has** rather than being told. The
# WebDAV and FTP roots live on a small disk on purpose -- that is what makes
# "the destination fills up" a real condition -- and a ten-gigabyte payload
# aimed at it would take every other suite down with it. A destination without
# room is a skip with the reason, printed, not a silent pass.
#
# Usage:
#   MOLE_TESTBED_ADDRESS=<address> MOLE_TESTBED_PASSWORD=<throwaway> make test-heavy
#   MOLE_TEST_HEAVY_BYTES=$((20 * 1024**3)) … make test-heavy
#
set -uo pipefail

ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"
PASSWORD="${MOLE_TESTBED_PASSWORD:-}"
S3_PORT="${MOLE_TESTBED_S3_PORT:-9000}"
S3_BUCKET="${MOLE_TESTBED_S3_BUCKET:-mole-testbed}"
REKEY_PORT="${MOLE_TESTBED_REKEY_PORT:-2222}"
BUILD="${1:-build/debug}"

# Ten gibibytes by default. Not a hundred: the machine this was written against
# has 27 GB free on its system disk and 80 GB here, so a hundred would not fit
# at either end -- and the check that matters is the *ratio* between the payload
# and the temporary space it needs, which ten gigabytes proves as well as a
# hundred. Ask for more with MOLE_TEST_HEAVY_BYTES when there is room for more.
BYTES="${MOLE_TEST_HEAVY_BYTES:-$((10 * 1024 * 1024 * 1024))}"

if [ -z "$ADDRESS" ] || [ -z "$PASSWORD" ]; then
    cat >&2 <<'MESSAGE'
Set MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD.

Neither is in this repository and neither should be: they live in the
environment directory named in CLAUDE.md. Without them this tier skips every
case that needs a server, which is a result rather than a pass -- run it anyway
to see the local-to-local case, which needs nothing.
MESSAGE
    exit 2
fi

export MOLE_TEST_SFTP_HOST="$ADDRESS" MOLE_TEST_SFTP_PORT=22
export MOLE_TEST_SFTP_USER="$ACCOUNT" MOLE_TEST_SFTP_PASS="$PASSWORD"
export MOLE_TEST_SFTP_BASE="/home/$ACCOUNT/sftp"
export MOLE_TEST_SFTP_REKEY_PORT="$REKEY_PORT"

export MOLE_TEST_WEBDAV_URL="http://$ADDRESS/dav"
export MOLE_TEST_WEBDAV_USER="$ACCOUNT" MOLE_TEST_WEBDAV_PASS="$PASSWORD"

export MOLE_TEST_FTP_HOST="$ADDRESS" MOLE_TEST_FTP_PORT=21
export MOLE_TEST_FTP_USER="$ACCOUNT" MOLE_TEST_FTP_PASS="$PASSWORD"
export MOLE_TEST_FTP_BASE="/Shared"

export MOLE_TEST_S3_ENDPOINT="http://$ADDRESS:$S3_PORT"
export MOLE_TEST_S3_BUCKET="$S3_BUCKET" MOLE_TEST_S3_REGION="${MOLE_TEST_S3_REGION:-us-east-1}"
export MOLE_TEST_S3_KEY_ID="${MOLE_TEST_S3_KEY_ID:-$ACCOUNT}"
export MOLE_TEST_S3_SECRET="${MOLE_TEST_S3_SECRET:-$PASSWORD}"

export MOLE_TEST_IGNORE_SELF_SIGNED_CERT=1
export MOLE_TEST_HEAVY_BYTES="$BYTES"
export MOLE_TEST_HEAVY_REPORT="${MOLE_TEST_HEAVY_REPORT:-$BUILD/heavy-report.txt}"

# On the stock port, which is the one the host-key case leaves alone: it rotates
# the second server's identity, and a channel arriving there would correctly
# refuse to talk to the machine for the rest of the run. The outage cases want
# the opposite -- they blackhole this port -- which is why they are the ones
# behind a variable, and why sorting that out (MOLE-109) means sorting out the
# port each one may use.
CONTROL="${MOLE_TEST_CONTROL:-ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $ACCOUNT@$ADDRESS sudo mole-control}"

# What each destination has room for, from the machine itself. Missing answers
# leave the cap unset, which means the suite goes ahead -- an absent control
# channel must not silently turn the tier off.
for service in sftp s3 webdav ftp; do
    room="$($CONTROL room "$service" 2>/dev/null | tr -dc '0-9')"
    [ -n "$room" ] || continue
    printf '  %-7s has %s free\n' "$service" "$(numfmt --to=iec "$room" 2>/dev/null || echo "$room bytes")"
    export "MOLE_TEST_HEAVY_CAP_$(echo "$service" | tr a-z A-Z)=$room"
done

echo
echo "payload: $(numfmt --to=iec "$BYTES" 2>/dev/null || echo "$BYTES bytes"), report: $MOLE_TEST_HEAVY_REPORT"
echo

export MOLE_TEST_CONTROL="$CONTROL"

cmake --build "$BUILD" --target tst_HeavyTransfers tst_Interference --parallel "$(nproc)" >/dev/null || exit 1

# Run the binary rather than ctest: this tier is watched while it runs, and one
# line per scenario as it happens is the point. ctest would hold every line
# until the end.
# QTest kills a test function after five minutes by default, which is less than
# one ten-gigabyte transfer takes on any link worth testing. The watchdog is
# still wanted -- a tier that hangs for ever is worse than one that fails -- so
# it is raised rather than removed.
QTEST_FUNCTION_TIMEOUT="${QTEST_FUNCTION_TIMEOUT:-7200000}" \
QT_QPA_PLATFORM=offscreen "$BUILD/tests/tst_HeavyTransfers"
status=$?

# And the same transfers with the machine being attacked while they run. After
# the scale tier rather than before it: this one leaves the link damaged for
# seconds at a time, and a rate limit still in place would make every throughput
# figure above a lie.
echo
QTEST_FUNCTION_TIMEOUT="${QTEST_FUNCTION_TIMEOUT:-7200000}" \
QT_QPA_PLATFORM=offscreen "$BUILD/tests/tst_Interference"
[ $? -eq 0 ] || status=$?

echo
echo "recorded in $MOLE_TEST_HEAVY_REPORT:"
tail -20 "$MOLE_TEST_HEAVY_REPORT" 2>/dev/null

# Leave nothing behind on the machine, whatever happened above.
$CONTROL restore >/dev/null 2>&1
exit $status
