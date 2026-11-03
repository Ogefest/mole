#!/usr/bin/env bash
#
# Runs every suite that needs a real server, against the machine the other two
# scripts built.
#
# The point is as much the report as the run. Each of these suites skips itself
# when there is nothing to talk to, and in practice that meant they had never
# run at all -- which is how a listing behaviour that differs between servers
# stayed hidden until a second server was tried. **A skip is a result**, and a
# silent one is exactly how this situation arose, so a suite that skips is named
# and counted here rather than blending into a row of green.
#
# Usage:
#   MOLE_TESTBED_ADDRESS=<address> MOLE_TESTBED_PASSWORD=<throwaway> make test-live
#
set -uo pipefail

ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"
PASSWORD="${MOLE_TESTBED_PASSWORD:-}"
S3_PORT="${MOLE_TESTBED_S3_PORT:-9000}"
S3_BUCKET="${MOLE_TESTBED_S3_BUCKET:-mole-testbed}"
KEY_ACCOUNT="${MOLE_TESTBED_KEY_ACCOUNT:-molekey}"
# The private half of the key services.sh put on that account. A path handed in
# from the environment directory, never a file in this repository and never a
# default that would make one: a key checked in is a key published.
KEY_FILE="${MOLE_TESTBED_SFTP_KEY:-}"
BUILD="${1:-build/debug}"

if [ -z "$ADDRESS" ] || [ -z "$PASSWORD" ]; then
    cat >&2 <<'MESSAGE'
Set MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD.

Neither is in this repository, and neither should be: they live in the
environment directory named in CLAUDE.md, under "Where work is tracked".
Without them these suites skip themselves, which is what they have always
done and what this target exists to stop being invisible.
MESSAGE
    exit 2
fi

# Everything each suite reads, derived from the two facts above so there is one
# place to change when the machine moves.
export MOLE_TEST_SFTP_HOST="$ADDRESS" MOLE_TEST_SFTP_PORT=22
export MOLE_TEST_SFTP_USER="$ACCOUNT" MOLE_TEST_SFTP_PASS="$PASSWORD"
export MOLE_TEST_SFTP_BASE="/home/$ACCOUNT/sftp"

# The account a key gets into and a password does not, when its key is on this
# machine. Separate from the one above on purpose: an account that still accepts
# a password cannot show that the key was what got in, which is the whole of what
# MOLE-423 is for. Only the readable case exports -- a path that is set but
# missing would turn a skip into a failure that says nothing about the code.
if [ -n "$KEY_FILE" ] && [ -r "$KEY_FILE" ]; then
    export MOLE_TEST_SFTP_KEY="$KEY_FILE"
    export MOLE_TEST_SFTP_KEY_USER="$KEY_ACCOUNT"
    export MOLE_TEST_SFTP_KEY_BASE="/home/$KEY_ACCOUNT/sftp"
elif [ -n "$KEY_FILE" ]; then
    echo "MOLE_TESTBED_SFTP_KEY names a file this machine cannot read: the" >&2
    echo "key-only case will skip. It lives with the other credentials for this" >&2
    echo "environment, outside the checkout." >&2
fi

# The Samba share services.sh puts on the machine. Named the same way as the
# rest: the address and the password come from the environment, never from here.
export MOLE_TEST_SMB_HOST="$ADDRESS"
export MOLE_TEST_SMB_SHARE="${MOLE_TESTBED_SMB_SHARE:-moledata}"
export MOLE_TEST_SMB_USER="$ACCOUNT" MOLE_TEST_SMB_PASS="$PASSWORD"

# The kernel's NFS export. No account and no password, because NFS has neither:
# the export list decides who may mount, which is why services.sh asks for an
# address to allow rather than a user to create.
export MOLE_TEST_NFS_HOST="$ADDRESS"
export MOLE_TEST_NFS_EXPORT="${MOLE_TESTBED_NFS_EXPORT:-/srv/moledata/nfs}"

export MOLE_TEST_WEBDAV_URL="http://$ADDRESS/dav"
export MOLE_TEST_WEBDAV_USER="$ACCOUNT" MOLE_TEST_WEBDAV_PASS="$PASSWORD"

export MOLE_TEST_FTP_HOST="$ADDRESS" MOLE_TEST_FTP_PORT=21
export MOLE_TEST_FTP_USER="$ACCOUNT" MOLE_TEST_FTP_PASS="$PASSWORD"
# The second FTP account, whose login directory is its home rather than `/`: the
# only place the addressing MOLE-349 fixed can be seen working. Same password,
# named the way services.sh names it. See MOLE-421.
FTP_HOME_ACCOUNT="${MOLE_TESTBED_FTP_HOME_ACCOUNT:-moleftphome}"
export MOLE_TEST_FTP_HOME_USER="$FTP_HOME_ACCOUNT" MOLE_TEST_FTP_HOME_PASS="$PASSWORD"
export MOLE_TEST_FTP_HOME_BASE="/home/$FTP_HOME_ACCOUNT/ftp"

export MOLE_TEST_S3_ENDPOINT="http://$ADDRESS:$S3_PORT"
export MOLE_TEST_S3_BUCKET="$S3_BUCKET" MOLE_TEST_S3_REGION="${MOLE_TEST_S3_REGION:-us-east-1}"
export MOLE_TEST_S3_KEY_ID="$ACCOUNT" MOLE_TEST_S3_SECRET="$PASSWORD"
export MOLE_TEST_S3_ADDRESSING=path
# The container that keeps earlier objects, derived the way the rest are. The one
# above goes on being the container *without* the feature, which is now
# load-bearing rather than incidental: it is what "a container without it
# contributes nothing" is held against.
export MOLE_TEST_S3_VERSIONED_BUCKET="${MOLE_TESTBED_S3_VERSIONED_BUCKET:-${S3_BUCKET}-versioned}"

# The testbed's certificate is honestly self-signed. TLS stays required; this
# says who vouches for it, and nothing about whether the connection is
# encrypted.
export MOLE_TEST_IGNORE_SELF_SIGNED_CERT=1

export QT_QPA_PLATFORM=offscreen

# **Every case has a deadline, and it is this tier's rather than QtTest's.** On
# 2026-09-05 a suite that was never going to finish held the release gate for
# twenty-five minutes, and nothing stopped it or said what it was waiting on, so
# the person watching could not tell a slow suite from a stuck one. The cause was
# MOLE-418; the next hang will have another. So each suite runs with QtTest's
# `-v1`, which names every case as it is entered, and this script watches that:
#
# - a case slower than HEARTBEAT seconds is named, and named again every
#   HEARTBEAT seconds after, so slow and stuck look different from outside long
#   before anything is decided;
# - a case that reaches CASE_DEADLINE seconds is stopped, with the whole suite,
#   and reported as a TIMEOUT naming it -- not as the assertion it happened to be
#   inside, and not as an environment nobody configured.
#
# **Six minutes, and why that number: twice the slowest case measured.** Most
# live cases answer in seconds. Two kinds do not. A case that cuts a connection
# mid-read waits for the transfer's budget to run out rather than for a socket
# to fail (ADR-0013's second amendment), which is about a minute and a half. And
# SFTP's aLargeFileGoesUpWhole, measured against the testbed on 2026-11-03 with
# this deadline's heartbeat, was still running at 150 s and done by 180 s. Four
# minutes was the first figure and that run is what moved it: one and a third
# times the slowest case is a bound that trips on a slower day. Six is double,
# and still a quarter of what the hang that prompted this cost. See MOLE-424.
CASE_DEADLINE="${MOLE_LIVE_CASE_DEADLINE:-360}"
HEARTBEAT="${MOLE_LIVE_HEARTBEAT:-30}"

# QtTest's own watchdog stays, above the deadline rather than below it, so it is
# only ever a backstop: when it fires the process aborts with a stack trace, and
# that is a worse report of the same fact.
export QTEST_FUNCTION_TIMEOUT="${QTEST_FUNCTION_TIMEOUT:-900000}"

# Runs one suite under the deadline. Sets `output` and `code` the way capturing
# the binary's output would, and `timed_out` to the case that was cut off, empty
# when none was.
#
# The suite gets a session of its own so that stopping it stops everything it
# started -- a child holding a connection is exactly what a stuck case leaves
# behind. Polled once a second rather than read line by line, because a case that
# is stuck prints nothing, and nothing is what this has to notice.
run_suite() {
    local binary="$1" log
    log=$(mktemp)
    setsid "$binary" -v1 > "$log" 2>&1 &
    local pid=$!
    local current="the start of the suite" entered=$SECONDS seen=0 lines next_beat=$HEARTBEAT
    timed_out=""
    while kill -0 "$pid" 2>/dev/null; do
        lines=$(wc -l < "$log")
        if [ "$lines" -gt "$seen" ]; then
            local latest
            latest=$(tail -n +"$((seen + 1))" "$log" | sed -n 's/^INFO *: \(.*()\) entering$/\1/p' | tail -1)
            seen=$lines
            if [ -n "$latest" ]; then
                current="$latest"
                entered=$SECONDS
                next_beat=$HEARTBEAT
            fi
        fi
        local in_case=$((SECONDS - entered))
        if [ "$in_case" -ge "$CASE_DEADLINE" ]; then
            timed_out="$current"
            kill -TERM -- "-$pid" 2>/dev/null
            local grace=0
            while kill -0 "$pid" 2>/dev/null && [ "$grace" -lt 5 ]; do
                sleep 1
                grace=$((grace + 1))
            done
            kill -KILL -- "-$pid" 2>/dev/null
            break
        fi
        if [ "$in_case" -ge "$next_beat" ]; then
            printf '          still in %s after %d s\n' "$current" "$in_case"
            next_beat=$((next_beat + HEARTBEAT))
        fi
        sleep 1
    done
    wait "$pid" 2>/dev/null
    code=$?
    output=$(cat "$log")
    rm -f "$log"
}

# The control channel, when it is installed. One case in the SFTP suite cuts a
# connection mid-read to prove that a truncated transfer does not read as a whole
# file, and without this it skips -- which this target reports as not green,
# because a suite that never met the server is what it exists to make visible.
# Every interference clears itself; see scripts/testbed/control.sh.
# It arrives over the third sshd, which no test attacks -- ADR-0054. A machine
# provisioned before that server existed still answers on port 22, and this tier
# does nothing to port 22, so falling back is safe here in a way it is not in
# test-heavy.sh.
CONTROL_PORT="${MOLE_TESTBED_CONTROL_PORT:-2022}"
if [ -z "${MOLE_TEST_CONTROL:-}" ]; then
    for port in "$CONTROL_PORT" 22; do
        if ssh -o BatchMode=yes -o ConnectTimeout=5 -p "$port" "$ACCOUNT@$ADDRESS" \
                'command -v mole-control' >/dev/null 2>&1; then
            export MOLE_TEST_CONTROL="ssh -o BatchMode=yes -p $port $ACCOUNT@$ADDRESS sudo mole-control"
            break
        fi
    done
fi

# What earlier runs left behind, taken away before this one starts rather than
# only after it. These suites clean up in `cleanup()`, which covers every case
# that reaches the end -- but the litter measured on 2026-08-19 was mostly theirs:
# fifty-four `mole-dav-*` collections and forty-odd `mole-smb-*` directories from
# runs cut short. It matters here for the same reason it matters in test-heavy.sh:
# these suites decline when a destination has no room, and room taken by our own
# leftovers makes that skip a lie. Any run already going is named so its payload
# survives -- see MOLE-235.
if [ -n "${MOLE_TEST_CONTROL:-}" ]; then
    running=$(pgrep -f "$BUILD/tests/tst_" 2>/dev/null | tr '\n' ' ')
    [ -z "$running" ] || echo "  sparing payloads of runs already going: $running"
    $MOLE_TEST_CONTROL sweep $running || true
fi

SUITES="tst_SftpFileSystem tst_WebdavFileSystem tst_FtpFileSystem tst_S3FileSystem tst_SmbFileSystem
tst_NfsFileSystem"

printf '\n\033[1mLive suites against %s\033[0m\n\n' "$ADDRESS"

failed=0
skipped=0
ran=0
for suite in $SUITES; do
    binary="$BUILD/tests/$suite"
    if [ ! -x "$binary" ]; then
        printf '  \033[31mMISSING\033[0m %-24s not built -- run make first\n' "$suite"
        failed=$((failed + 1))
        continue
    fi

    run_suite "$binary"
    totals=$(printf '%s' "$output" | sed -n 's/^Totals: //p' | tail -1)
    # A suite that skipped every case it has is a suite that never met the
    # server, whatever its exit status says.
    skips=$(printf '%s' "$output" | grep -c '^SKIP' || true)

    if [ -n "$timed_out" ]; then
        printf '  \033[31mTIMEOUT\033[0m %-24s %s did not finish within %d s\n' \
            "$suite" "$timed_out" "$CASE_DEADLINE"
        failed=$((failed + 1))
    elif [ "$code" -ne 0 ]; then
        printf '  \033[31mFAIL\033[0m    %-24s %s\n' "$suite" "$totals"
        printf '%s\n' "$output" | grep '^FAIL' | sed 's/^/          /'
        failed=$((failed + 1))
    elif [ "$skips" -gt 0 ]; then
        printf '  \033[33mSKIP\033[0m    %-24s %s\n' "$suite" "$totals"
        printf '%s\n' "$output" | grep -A1 '^SKIP' | sed 's/^/          /' | head -4
        skipped=$((skipped + 1))
    else
        printf '  \033[32mok\033[0m      %-24s %s\n' "$suite" "$totals"
        ran=$((ran + 1))
    fi
done

printf '\n  %d ran, %d skipped, %d failed\n\n' "$ran" "$skipped" "$failed"

# A skip is a result, and a run that quietly skipped everything is not a pass.
# Said by what happened rather than by one sentence for both: a run with a case
# cut off at the deadline used to close by blaming a skip it never had.
if [ "$failed" -gt 0 ]; then
    printf '\033[31mNot green.\033[0m A suite failed, was stopped at the deadline, or was not built.\n\n'
    exit 1
fi
if [ "$skipped" -gt 0 ]; then
    printf '\033[31mNot green.\033[0m A suite that skipped never met the server, which is the\n'
    printf 'thing this target exists to make visible.\n\n'
    exit 1
fi
printf '\033[32mEvery live suite ran, against a real server.\033[0m\n\n'
