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
# **It asks the machine how much room it has** rather than being told, and sends
# each destination as much of the payload as that destination can hold. The WebDAV
# and FTP roots live on a small disk on purpose -- that is what makes "the
# destination fills up" a real condition, and scripts/testbed/README.md says so --
# and a ten-gigabyte payload aimed at it would take every other suite down with it.
#
# **Those two right answers used to be incompatible.** The payload was one figure for
# all four destinations, so the two on the small disk skipped, and `make release`
# refused itself on the skips -- correctly, the first time the gate was ever run. What
# each destination gets is now half the room it reports, capped at the payload asked
# for; a destination with no room worth using is still a skip with the reason,
# printed, never a silent pass. The reasoning is in tests/scale/HeavyPayload.h beside
# the arithmetic, and MOLE-320 is the ticket.
#
# **What it costs, measured on 2026-09-01 against the testbed as provisioned**, because
# anybody about to run the release gate wants to know before rather than after: the
# scale tier took 71 minutes at the ten-gibibyte default and the interference tier 26,
# so the whole gate -- fast, live and heavy -- is about an hour and three quarters.
# Twenty-eight of those minutes are the sftp-rekey download, at 5,7 MiB/s: a read from
# a server that re-keys inside the span pays a stall-guard wait at every re-key point
# (ADR-0013's amendment), and it is the one destination where the payload size costs
# real time. Everything else moved at 15 to 111 MiB/s.
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

# **What a single request may carry, which is not the same question as how much room
# a destination has** -- and for WebDAV it is the smaller of the two.
#
# Apache 2.4 refuses a request body over one gibibyte and answers 413, from its own
# default rather than from anything in this machine's configuration: `LimitRequestBody`
# has defaulted to 1 GiB since 2.4.54. Measured against the machine on 2026-09-01 --
# exactly 1073741824 bytes is accepted with a 201, and one byte more is refused with a
# 413 before the body is read at all. A WebDAV upload is one request, because the
# protocol has no ranged PUT, so this is a ceiling on the file and not on a chunk of it.
#
# **It cost a heavy tier run to find, and why it was not obvious is worth keeping:**
# Mole reports a 413 as "no room left on the server", so a 413 against a destination
# that happens to live on a small disk reads exactly like the disk filling up. The room
# figure said 3,64 GiB free and the transfer stopped at 1,01 GiB, which sends any reader
# straight to the disk. See MOLE-320.
#
# Named per destination rather than as one number, because it is a fact about a server:
# set MOLE_TEST_HEAVY_MAX_SFTP, _S3 or _FTP for one that has a limit of its own.
export MOLE_TEST_HEAVY_MAX_WEBDAV="${MOLE_TEST_HEAVY_MAX_WEBDAV:-$((1024 * 1024 * 1024))}"
export MOLE_TEST_HEAVY_REPORT="${MOLE_TEST_HEAVY_REPORT:-$BUILD/heavy-report.txt}"

# On the control channel's own port, which is the whole of the answer to "which
# port may this arrive over".
#
# Every other port on that machine is something a case in this tier attacks. The
# stock one is blackholed by the outage cases and stopped by the restart case;
# the second server's host key is rotated. A channel on either would be cut by
# the tier it is meant to be driving -- which is what used to happen, and why the
# outage cases were behind a variable. The third sshd is attacked by nothing and
# `mole-control` refuses to be pointed at it. See ADR-0054.
CONTROL_PORT="${MOLE_TESTBED_CONTROL_PORT:-2022}"
if [ -z "${MOLE_TEST_CONTROL:-}" ] \
        && ! ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
            -p "$CONTROL_PORT" "$ACCOUNT@$ADDRESS" true 2>/dev/null; then
    echo "Nothing answers on port $CONTROL_PORT: the control sshd is not there." >&2
    echo "Run scripts/testbed/services.sh. This tier blackholes port 22, so a" >&2
    echo "control channel on port 22 would be cut along with the transfer." >&2
    exit 2
fi
CONTROL="${MOLE_TEST_CONTROL:-ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -p $CONTROL_PORT $ACCOUNT@$ADDRESS sudo mole-control}"

# Before the tier and not only after it, because the runs this is for never
# reached the end. Both tiers clean up in `cleanup()`, which works for every run
# that finishes a case; a run killed by a watchdog, by SIGABRT, by Ctrl-C or by the
# machine going away leaves its payload where it was, and nothing used to take it
# away. Nineteen gigabytes in twenty-five files had built up over two days when
# MOLE-235 was written, and the cost is not untidiness: the room check below then
# reports a skip for want of space that is really our own litter, so the tier goes
# green for having done nothing.
#
# Any test binary already running is named, so its payload is spared -- two tiers
# started side by side must not delete each other's work. `mole-control` also
# spares anything a server currently holds open, which is what covers a run
# started from another machine, whose pids mean nothing here.
running=$(pgrep -f "$BUILD/tests/tst_" 2>/dev/null | tr '\n' ' ')
[ -z "$running" ] || echo "  sparing payloads of runs already going: $running"
$CONTROL sweep $running || echo "  the sweep did not run; the room figures below may be our own litter" >&2

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
# Kept in a variable first: `[ $? -eq 0 ] || status=$?` reads the *test command's*
# status the second time, not the binary's, so an interference failure was reported as
# 1 whatever it actually exited with -- and if the scale tier had already failed, this
# overwrote its status with the same 1. Nothing noticed because both are non-zero and
# the gate only asks whether the tier was green. Found while reading this file for
# MOLE-320.
interference=$?
[ "$interference" -eq 0 ] || status=$interference

echo
echo "recorded in $MOLE_TEST_HEAVY_REPORT:"
tail -20 "$MOLE_TEST_HEAVY_REPORT" 2>/dev/null

# Leave nothing behind on the machine, whatever happened above.
$CONTROL restore >/dev/null 2>&1
exit $status
