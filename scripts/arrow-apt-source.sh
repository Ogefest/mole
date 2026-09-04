#!/usr/bin/env bash
#
# Adds Apache's own Arrow repository, having checked who signed what it adds.
#
# **Arrow is in no Ubuntu archive at any version**, so the Parquet grid can only
# come from Apache, and the way in is a .deb that installs a signing key and a
# source list. Two workflows did that with a bare `wget | apt-get install` -- a
# package fetched over a URL and given root, with nothing asked about it. Beside
# `scripts/qt-tsan.sh`, which keeps a SHA-256 table and says of itself that
# "a source tarball fetched over the network and then compiled is exactly the
# thing to check rather than trust", that was the odd one out. See MOLE-390.
#
# **A digest cannot be the check here, and that is the interesting part.** The
# file is published as `apache-arrow-apt-source-latest-<codename>.deb`: `latest`
# is the name, so the bytes change whenever Apache cuts a release, and a recorded
# digest would turn every one of those into a red pipeline. Apache publishes a
# detached signature beside it, which is the check that survives a republish: an
# attacker who can put bytes on the mirror cannot sign them.
#
# So the gate is a good signature from a key in Apache Arrow's own published KEYS
# file, fetched over TLS from downloads.apache.org, and the digest is *recorded*
# rather than enforced -- printed, and compared with the one seen when this was
# written, so a change is visible in the log of a run that was nonetheless
# verified.
#
# Usage:
#   scripts/arrow-apt-source.sh            # adds the repository
#
# Sudo is used for the parts that need root, so this runs on a runner the same way
# it runs on a machine where the caller is not root.
#
set -uo pipefail

die() { printf 'arrow-apt-source: %s\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }

# The signature seen on 2026-09-04, and the digest of the noble package on the
# same day. Neither is a secret and neither is a pin that has to be right for the
# repository to be added: the fingerprint is what a good signature is checked
# against, and a key rotation is reported rather than fatal, because Apache
# rotating a release manager's key is ordinary and a bad signature is not.
KEY_FINGERPRINT="7BD1A9A50E4F701BCBFD90EF9E922B2D60E9FD1C"
KEYS_URL="https://downloads.apache.org/arrow/KEYS"

command -v gpg >/dev/null 2>&1 || die "gpg is needed to check the signature"
command -v lsb_release >/dev/null 2>&1 || die "lsb_release is needed to name the distribution"

distribution=$(lsb_release --id --short | tr 'A-Z' 'a-z')
codename=$(lsb_release --codename --short)
package="apache-arrow-apt-source-latest-$codename.deb"
base="https://packages.apache.org/artifactory/arrow/$distribution"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

note "$base/$package"
curl -sSfL -o "$work/$package" "$base/$package" || die "could not download $package"
curl -sSfL -o "$work/$package.asc" "$base/$package.asc" \
    || die "there is no signature beside $package, and an unsigned one is not installed here"

# A keyring of its own, so nothing is imported into whoever is running this.
export GNUPGHOME="$work/gnupg"
mkdir -m 700 "$GNUPGHOME"
curl -sSfL "$KEYS_URL" | gpg --batch --quiet --import \
    || die "could not import Apache Arrow's KEYS from $KEYS_URL"

status=$(gpg --batch --status-fd 1 --verify "$work/$package.asc" "$work/$package" 2>/dev/null)
signed_by=$(printf '%s\n' "$status" | sed -n 's/^\[GNUPG:\] VALIDSIG \([0-9A-F]*\) .*/\1/p')
[ -n "$signed_by" ] || die "the signature on $package is not good against $KEYS_URL"
note "signed by $signed_by"
if [ "$signed_by" != "$KEY_FINGERPRINT" ]; then
    note "which is not the key recorded in this script ($KEY_FINGERPRINT)."
    note "It is in Apache's own KEYS, so this carries on -- but a rotation is worth"
    note "a look, and the line in this script is worth updating with it."
fi
note "sha256 $(sha256sum "$work/$package" | cut -d' ' -f1)"

sudo apt-get install -y "$work/$package" || die "apt-get refused $package"
sudo apt-get update || die "apt-get update failed after adding the repository"
sudo apt-get install -y --no-install-recommends libarrow-dev libparquet-dev \
    || die "the repository was added but Arrow could not be installed from it"
note "libarrow-dev and libparquet-dev installed"
