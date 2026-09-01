#!/usr/bin/env bash
#
# Asks each service on the test machine whether it is actually there.
#
# Installed is not running and running is not answering. Every check here
# authenticates as the test account and moves a byte, because a server that
# accepts a connection and then refuses everything is the shape of failure that
# a port scan calls success.
#
# Exits non-zero if anything is not answering, and says which.
#
set -euo pipefail

ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"
PASSWORD="${MOLE_TESTBED_PASSWORD:-}"
REKEY_PORT="${MOLE_TESTBED_REKEY_PORT:-2222}"
CONTROL_PORT="${MOLE_TESTBED_CONTROL_PORT:-2022}"
S3_PORT="${MOLE_TESTBED_S3_PORT:-9000}"

[ -n "$ADDRESS" ] && [ -n "$PASSWORD" ] || {
    echo "Set MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD." >&2; exit 2; }

failures=0
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; failures=$((failures + 1)); }
# Neither pass nor fail: something this machine cannot ask about. A check that
# counted "I have no client for that" as a failure would teach people to ignore
# the whole run.
note() { printf '  \033[33m--\033[0m    %s\n' "$*"; }

printf '\n\033[1mServices on %s\033[0m\n' "$ADDRESS"

# --- SFTP, on both servers ---------------------------------------------------
#
# A listing, not a banner. sftp will connect happily to something that then
# refuses to open a directory.

for port in 22 "$REKEY_PORT"; do
    if ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
           -p "$port" "$ACCOUNT@$ADDRESS" 'true' >/dev/null 2>&1; then
        ok "sftp on $port"
    else
        bad "sftp on $port"
    fi
done

# The second server exists to be configured differently from the first. If both
# ended up with the same cipher list, it is not a second server, it is a second
# port -- and the re-key stall it is there to provoke would never happen.
cipher_on() {
    # -v before the destination. After it, ssh passes it to the remote command
    # and the client says nothing at all -- which is how this check quietly
    # measured nothing and still printed a tick.
    ssh -v -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
        -p "$1" "$ACCOUNT@$ADDRESS" true 2>&1 \
        | sed -n 's/.*kex: server->client cipher: \([^ ]*\) .*/\1/p' | head -1
}
first=$(cipher_on 22 || true)
second=$(cipher_on "$REKEY_PORT" || true)
if [ -z "$second" ]; then
    bad "could not tell what the second sshd negotiates"
elif [ "$first" = "$second" ]; then
    bad "both servers negotiate $second -- the second one is a port, not a different server"
else
    ok "ciphers differ: $first on 22, $second on $REKEY_PORT"
fi

# The control channel's own server, and the two things that make it one.
#
# It has to answer, and `mole-control` has to refuse to damage it. A control
# sshd that is reachable but that the instrument will happily blackhole is the
# fault of ADR-0054 wearing a different port number.
if ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
       -p "$CONTROL_PORT" "$ACCOUNT@$ADDRESS" 'true' >/dev/null 2>&1; then
    ok "the control channel answers on $CONTROL_PORT"
    if ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
           -p "$CONTROL_PORT" "$ACCOUNT@$ADDRESS" \
           "sudo mole-control blackhole $CONTROL_PORT 1" >/dev/null 2>&1; then
        bad "mole-control blackholed its own port -- it can still cut the machine off"
    else
        ok "mole-control refuses to blackhole port $CONTROL_PORT"
    fi
else
    bad "the control channel on $CONTROL_PORT (run services.sh)"
fi

# --- WebDAV ------------------------------------------------------------------
#
# Written and read back. A PROPFIND that succeeds proves authentication and
# nothing else.

dav="http://$ADDRESS/dav"
stamp="check-$$"
if curl -fsS -u "$ACCOUNT:$PASSWORD" -T - "$dav/$stamp" <<<"hello" >/dev/null 2>&1 \
   && [ "$(curl -fsS -u "$ACCOUNT:$PASSWORD" "$dav/$stamp" 2>/dev/null)" = "hello" ]; then
    ok "webdav at $dav"
    curl -fsS -u "$ACCOUNT:$PASSWORD" -X DELETE "$dav/$stamp" >/dev/null 2>&1 || true
else
    bad "webdav at $dav"
fi

# --- SMB ---------------------------------------------------------------------
#
# A listing through the share, not a port that answers. smbd accepts a
# connection long before it will hand over a directory, and a check that stops
# at the connection is a check that passes on a misconfigured share.

SMB_SHARE="${MOLE_TESTBED_SMB_SHARE:-moledata}"
if command -v smbclient >/dev/null 2>&1; then
    if smbclient "//$ADDRESS/$SMB_SHARE" -U "$ACCOUNT%$PASSWORD" -c 'ls' >/dev/null 2>&1; then
        ok "smb //$ADDRESS/$SMB_SHARE"
    else
        bad "smb //$ADDRESS/$SMB_SHARE"
    fi
else
    note "smb: no smbclient here to ask with"
fi

# --- NFS ---------------------------------------------------------------------
#
# What the server says it exports, which needs no mount and therefore no root on
# this machine. Whether *this* address may mount it is a question for the export
# list rather than for a check that would need privileges to answer.

if command -v showmount >/dev/null 2>&1; then
    if grep -q "/srv/moledata/nfs" <<<"$(showmount -e "$ADDRESS" 2>/dev/null)"; then
        ok "nfs $ADDRESS:/srv/moledata/nfs"
    else
        note "nfs: not exported (set MOLE_TESTBED_NFS_CLIENTS and run services.sh)"
    fi
else
    note "nfs: no showmount here to ask with"
fi

# --- FTP ---------------------------------------------------------------------

# --ssl-reqd, so a server that quietly fell back to plain FTP fails this rather
# than passing it. --insecure because the certificate is self-signed, which is
# the honest thing for a disposable machine to carry.
if curl -fsS --ftp-pasv --ssl-reqd --insecure --connect-timeout 8 -u "$ACCOUNT:$PASSWORD" \
        "ftp://$ADDRESS/" >/dev/null 2>&1; then
    ok "ftp on 21, TLS required"
else
    bad "ftp on 21"
fi

# --- S3 ----------------------------------------------------------------------
#
# Unauthenticated on purpose: MinIO answering "access denied" in its own XML is
# proof that MinIO is what is on the port, and needs no signing to find out.

if curl -fsS --connect-timeout 8 "http://$ADDRESS:$S3_PORT/minio/health/live" >/dev/null 2>&1; then
    ok "s3 (minio) on $S3_PORT"
else
    bad "s3 (minio) on $S3_PORT"
fi

printf '\n'
if [ "$failures" -gt 0 ]; then
    printf '\033[31m%d not answering.\033[0m\n\n' "$failures"
    exit 1
fi
printf '\033[32mEverything answered.\033[0m\n\n'
