#!/usr/bin/env bash
#
# Puts the four protocols on the machine provision.sh built.
#
# The implementations people actually run, not stand-ins: OpenSSH, Apache's
# mod_dav, MinIO and vsftpd. A backend that has only ever been held against a
# fake has been tested against our idea of the protocol rather than against the
# protocol.
#
# Separate from provision.sh on purpose. A script that builds a machine and also
# configures four servers is a script nobody reads, and these are the parts most
# likely to be changed one at a time.
#
# Usage:
#   MOLE_TESTBED_ADDRESS=<address> MOLE_TESTBED_PASSWORD=<throwaway> \
#       scripts/testbed/services.sh
#
set -euo pipefail

ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"
PASSWORD="${MOLE_TESTBED_PASSWORD:-}"
# The second sshd. Configured to re-key sooner than any real server would, so
# the stall that ADR-0013 is about can be provoked on demand rather than waited
# for -- and so the fix can be held against a server that provokes it and one
# that does not.
REKEY_PORT="${MOLE_TESTBED_REKEY_PORT:-2222}"
REKEY_LIMIT="${MOLE_TESTBED_REKEY_LIMIT:-256M}"
S3_PORT="${MOLE_TESTBED_S3_PORT:-9000}"
# A bucket to aim at. Invented, like every other name in this repository.
S3_BUCKET="${MOLE_TESTBED_S3_BUCKET:-mole-testbed}"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
die() { printf '\n%s\n' "$*" >&2; exit 1; }

[ -n "$ADDRESS" ] || die "Set MOLE_TESTBED_ADDRESS to the machine provision.sh built.
It is not in this repository; it is in the environment directory named in
CLAUDE.md."
[ -n "$PASSWORD" ] || die "Set MOLE_TESTBED_PASSWORD -- the same throwaway the
machine was built with. FTP and WebDAV authenticate with it."
case "$PASSWORD" in
    *\'*) die "MOLE_TESTBED_PASSWORD cannot contain a single quote." ;;
esac

on_server() { ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$ACCOUNT@$ADDRESS" "sudo bash -s"; }

say "Packages"
on_server <<'REMOTE' || die "could not install the servers"
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq openssh-server apache2 apache2-utils vsftpd curl >/dev/null
REMOTE
note "OpenSSH, Apache, vsftpd"

# --- the roots ---------------------------------------------------------------
#
# WebDAV and FTP go on the small disk, because "the destination filled up" is a
# condition those two have to be able to produce. SFTP stays in the home
# directory on the system disk: filling that would take the machine with it.

say "Roots"
on_server <<REMOTE || die "could not make the roots"
set -euo pipefail
mountpoint -q /srv/moledata || { echo "the small disk is not mounted"; exit 1; }
for dir in webdav ftp ftp/Shared; do
    mkdir -p /srv/moledata/\$dir
    chown $ACCOUNT:$ACCOUNT /srv/moledata/\$dir
done
mkdir -p /home/$ACCOUNT/sftp
chown $ACCOUNT:$ACCOUNT /home/$ACCOUNT/sftp
REMOTE
note "/srv/moledata/webdav, /srv/moledata/ftp (small disk)"
note "/home/$ACCOUNT/sftp (system disk)"

# --- SFTP, and a second sshd that re-keys ------------------------------------

say "SFTP"
on_server <<REMOTE || die "could not configure the second sshd"
set -euo pipefail
install -d -m 0755 /etc/ssh/rekey

# A whole second server rather than a second Port line: the point is a *server
# configured differently*, and one sshd cannot offer two cipher lists at once.
cat > /etc/ssh/sshd_config.rekey <<'CONF'
Port $REKEY_PORT
HostKey /etc/ssh/ssh_host_ed25519_key
HostKey /etc/ssh/ssh_host_rsa_key
PidFile /run/sshd-rekey.pid
Subsystem sftp /usr/lib/openssh/sftp-server
PasswordAuthentication yes
PubkeyAuthentication yes
# The two settings this server exists for. A cipher with a block size under
# sixteen bytes re-keys at 2^30 whatever RekeyLimit says, and this makes it
# happen far sooner than that so a test does not have to move a gigabyte to see
# it. See docs/adr/0013-a-large-sftp-read-arrives-in-spans.md.
Ciphers chacha20-poly1305@openssh.com
RekeyLimit $REKEY_LIMIT
CONF
sed -i "s|^Port .*|Port $REKEY_PORT|" /etc/ssh/sshd_config.rekey

cat > /etc/systemd/system/sshd-rekey.service <<'UNIT'
[Unit]
Description=A second sshd that re-keys early, for the Mole live tests
After=network.target

[Service]
ExecStart=/usr/sbin/sshd -D -f /etc/ssh/sshd_config.rekey
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT

# The stock server needs to take a password too: the suite authenticates that
# way, because a key is a thing to distribute and this machine is disposable.
sed -i 's/^#\?PasswordAuthentication .*/PasswordAuthentication yes/' /etc/ssh/sshd_config

# And it is pinned to a cipher with a sixteen-byte block, which is the whole
# point of there being two servers.
#
# Left to itself OpenSSH prefers chacha20-poly1305 on both, so the "second
# server configured differently" was a second port running the identical
# configuration -- and the re-key stall it exists to provoke would have been
# provoked on both or neither. A sixteen-byte block does not re-key at 2^30, so
# this one is the control.
if ! grep -q '^Ciphers ' /etc/ssh/sshd_config; then
    echo 'Ciphers aes256-gcm@openssh.com,aes128-gcm@openssh.com' >> /etc/ssh/sshd_config
else
    sed -i 's|^Ciphers .*|Ciphers aes256-gcm@openssh.com,aes128-gcm@openssh.com|' /etc/ssh/sshd_config
fi
sshd -t -f /etc/ssh/sshd_config || { echo "the stock sshd config is broken; not restarting"; exit 1; }
systemctl restart ssh
systemctl daemon-reload
systemctl enable --now sshd-rekey >/dev/null
REMOTE
note "port 22: aes256-gcm, sixteen-byte block, does not re-key at 2^30"
note "port $REKEY_PORT: chacha20-poly1305, RekeyLimit $REKEY_LIMIT"

# --- WebDAV ------------------------------------------------------------------

say "WebDAV"
on_server <<REMOTE || die "could not configure Apache"
set -euo pipefail
a2enmod dav dav_fs auth_basic authn_file authz_user >/dev/null

cat > /etc/apache2/sites-available/moledav.conf <<'CONF'
Alias /dav /srv/moledata/webdav
<Directory /srv/moledata/webdav>
    DAV On
    Options Indexes
    AuthType Basic
    AuthName "mole"
    AuthUserFile /etc/apache2/moledav.passwd
    Require valid-user
</Directory>
CONF

htpasswd -bc /etc/apache2/moledav.passwd $ACCOUNT '$PASSWORD' >/dev/null 2>&1
chown www-data:www-data /etc/apache2/moledav.passwd
chmod 0640 /etc/apache2/moledav.passwd
# Apache writes its lock database and the files themselves as www-data, so the
# root has to belong to it rather than to the account that reads it over ssh.
chown -R www-data:www-data /srv/moledata/webdav
a2ensite moledav >/dev/null
systemctl reload apache2 || systemctl restart apache2
REMOTE
note "http://$ADDRESS/dav"

# --- FTP ---------------------------------------------------------------------

say "FTP"
on_server <<REMOTE || die "could not configure vsftpd"
set -euo pipefail
# A self-signed certificate, so FTP is FTPS. The suite requires TLS unless
# explicitly told not to, and it is right to: plain FTP is not what anybody
# should run, and the backend has a TLS path that deserves exercising.
# Self-signed because this machine is disposable and a real certificate for it
# would be a lie about what it is.
if [ ! -f /etc/ssl/private/vsftpd.pem ]; then
    # Two files, then joined. Passing the same path to -keyout and -out writes
    # the key and then overwrites it with the certificate.
    openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
        -subj '/CN=mole-testbed' \
        -keyout /tmp/vsftpd.key -out /tmp/vsftpd.crt >/dev/null 2>&1
    cat /tmp/vsftpd.key /tmp/vsftpd.crt > /etc/ssl/private/vsftpd.pem
    rm -f /tmp/vsftpd.key /tmp/vsftpd.crt
    chmod 0600 /etc/ssl/private/vsftpd.pem
fi

cat > /etc/vsftpd.conf <<'CONF'
listen=YES
listen_ipv6=NO
anonymous_enable=NO
local_enable=YES
write_enable=YES
local_umask=022
dirmessage_enable=YES
use_localtime=YES
xferlog_enable=YES
connect_from_port_20=YES
chroot_local_user=YES
allow_writeable_chroot=YES
secure_chroot_dir=/var/run/vsftpd/empty
pam_service_name=vsftpd
# Passive, and a small fixed range: a test behind anything at all needs to know
# which ports to expect rather than discovering them one failure at a time.
pasv_enable=YES
pasv_min_port=30000
pasv_max_port=30020
local_root=/srv/moledata/ftp
# FTPS, explicit rather than implicit -- which is what curl and everything else
# expects on port 21.
ssl_enable=YES
rsa_cert_file=/etc/ssl/private/vsftpd.pem
rsa_private_key_file=/etc/ssl/private/vsftpd.pem
allow_anon_ssl=NO
force_local_data_ssl=YES
force_local_logins_ssl=YES
ssl_tlsv1=YES
ssl_sslv2=NO
ssl_sslv3=NO
# Off, because curl opens the data connection without resuming the control
# connection's session and vsftpd would refuse it. A real server may well want
# this on; a test server that rejects the client under test is no use.
require_ssl_reuse=NO
# TLS 1.2, and no higher.
#
# vsftpd's shutdown handling predates TLS 1.3 and misreads a data connection
# closing cleanly: the bytes all arrive, the server writes "FAIL UPLOAD" to its
# own log and answers 426, and the client is told the transfer was aborted. It
# happens to *some* transfers and not others with no pattern in the size, which
# is the worst way for a fault to behave -- an intermittent failure in the test
# environment teaches everyone to re-run rather than to look.
#
# Reproduced with plain curl and no Mole involved, so it is the server rather
# than the client under test.
ssl_ciphers=ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256:HIGH
CONF
mkdir -p /var/run/vsftpd/empty

# The cipher list above does not hold TLS 1.3 back on its own -- OpenSSL keeps
# its 1.3 suites in a separate list and offers them whatever ssl_ciphers says
# -- so the ceiling goes where OpenSSL reads it. Scoped to vsftpd's own service
# rather than to /etc/ssl/openssl.cnf, which everything on the machine shares.
mkdir -p /etc/systemd/system/vsftpd.service.d
cat > /etc/systemd/system/vsftpd.service.d/tls12.conf <<'UNIT'
[Service]
Environment=OPENSSL_CONF=/etc/ssl/vsftpd-openssl.cnf
UNIT
cat > /etc/ssl/vsftpd-openssl.cnf <<'SSLCONF'
openssl_conf = default_conf

[default_conf]
ssl_conf = ssl_sect

[ssl_sect]
system_default = system_default_sect

[system_default_sect]
MaxProtocol = TLSv1.2
SSLCONF
systemctl daemon-reload

# Stopped, waited for, then started -- not restarted.
#
# vsftpd does not set SO_REUSEADDR, so a start that treads on the previous
# instance's port cannot bind. It then exits with status 2 and says nothing at
# all: no message on stderr, none in the journal, none on the console. That
# silence cost an evening, because every foreground attempt to reproduce it was
# masked by the very service being diagnosed still holding port 21.
systemctl stop vsftpd 2>/dev/null || true
# Escaped, so the far side counts. Left unescaped the local shell expands it
# first and puts a newline between every number, which ends the for-list on the
# first one and is a syntax error on arrival.
for _ in \$(seq 1 20); do
    ss -ltn 2>/dev/null | grep -q ':21 ' || break
    sleep 1
done
systemctl start vsftpd

# Started, not merely asked to start.
sleep 2
systemctl is-active --quiet vsftpd || { echo "vsftpd did not stay up:"; journalctl -u vsftpd -n 15 --no-pager; exit 1; }
REMOTE
note "ftps://$ADDRESS/ (root /srv/moledata/ftp, passive 30000-30020)"
note "  and /Shared inside it, which is where tst_FtpFileSystem works by default"

# --- S3 ----------------------------------------------------------------------
#
# MinIO rather than a bill. Its store goes on the system disk: a bucket has no
# business filling the disk that "the destination is full" is measured on.

say "S3"
on_server <<REMOTE || die "could not install MinIO"
set -euo pipefail
if [ ! -x /usr/local/bin/minio ]; then
    curl -fsSL -o /usr/local/bin/minio https://dl.min.io/server/minio/release/linux-amd64/minio
    chmod +x /usr/local/bin/minio
fi
id minio >/dev/null 2>&1 || useradd --system --home /var/lib/minio --shell /usr/sbin/nologin minio
mkdir -p /var/lib/minio
chown minio:minio /var/lib/minio

cat > /etc/default/minio <<'CONF'
MINIO_ROOT_USER=$ACCOUNT
MINIO_ROOT_PASSWORD=$PASSWORD
CONF
chmod 0640 /etc/default/minio

cat > /etc/systemd/system/minio.service <<'UNIT'
[Unit]
Description=MinIO, for the Mole live tests
After=network.target

[Service]
User=minio
Group=minio
EnvironmentFile=/etc/default/minio
ExecStart=/usr/local/bin/minio server /var/lib/minio --address :$S3_PORT
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable --now minio >/dev/null

# The bucket. MinIO on a single drive keeps each one as a top-level directory,
# so making it is making a directory -- and a suite that has to create its own
# bucket before it can test anything is a suite that fails for the wrong reason
# the first time it runs.
mkdir -p /var/lib/minio/$S3_BUCKET
chown -R minio:minio /var/lib/minio
REMOTE
note "http://$ADDRESS:$S3_PORT, bucket $S3_BUCKET"

say "Ready"
note "Check them with scripts/testbed/check-services.sh"
printf '\n  The addresses and the password belong in the environment directory,\n'
printf '  not in this repository. Next: issue #21, the control channel.\n\n'
