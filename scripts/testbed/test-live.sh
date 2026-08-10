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

export MOLE_TEST_WEBDAV_URL="http://$ADDRESS/dav"
export MOLE_TEST_WEBDAV_USER="$ACCOUNT" MOLE_TEST_WEBDAV_PASS="$PASSWORD"

export MOLE_TEST_FTP_HOST="$ADDRESS" MOLE_TEST_FTP_PORT=21
export MOLE_TEST_FTP_USER="$ACCOUNT" MOLE_TEST_FTP_PASS="$PASSWORD"

export MOLE_TEST_S3_ENDPOINT="http://$ADDRESS:$S3_PORT"
export MOLE_TEST_S3_BUCKET="$S3_BUCKET" MOLE_TEST_S3_REGION="${MOLE_TEST_S3_REGION:-us-east-1}"
export MOLE_TEST_S3_KEY_ID="$ACCOUNT" MOLE_TEST_S3_SECRET="$PASSWORD"
export MOLE_TEST_S3_ADDRESSING=path

# The testbed's certificate is honestly self-signed. TLS stays required; this
# says who vouches for it, and nothing about whether the connection is
# encrypted.
export MOLE_TEST_IGNORE_SELF_SIGNED_CERT=1

export QT_QPA_PLATFORM=offscreen

SUITES="tst_SftpFileSystem tst_WebdavFileSystem tst_FtpFileSystem tst_S3FileSystem"

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
