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

# QtTest's own watchdog, which is five minutes per test function and is not
# enough here. A case that cuts a connection mid-read now waits for the
# transfer's budget to run out rather than for a socket to fail (ADR-0013's
# second amendment), and against a real server over a real link that is a minute
# and a half on its own. Left at the default it reads as a hang, which is the
# opposite of what this target is for.
export QTEST_FUNCTION_TIMEOUT="${QTEST_FUNCTION_TIMEOUT:-900000}"

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

    output=$("$binary" 2>&1)
    code=$?
    totals=$(printf '%s' "$output" | sed -n 's/^Totals: //p' | tail -1)
    # A suite that skipped every case it has is a suite that never met the
    # server, whatever its exit status says.
    skips=$(printf '%s' "$output" | grep -c '^SKIP' || true)

    if [ "$code" -ne 0 ]; then
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
if [ "$failed" -gt 0 ] || [ "$skipped" -gt 0 ]; then
    printf '\033[31mNot green.\033[0m A suite that skipped never met the server, which is the\n'
    printf 'thing this target exists to make visible.\n\n'
    exit 1
fi
printf '\033[32mEvery live suite ran, against a real server.\033[0m\n\n'
