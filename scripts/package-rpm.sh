#!/usr/bin/env bash
#
# Builds the .rpm on the family it is for, in a container.
#
# **Not on this machine, and that is a finding rather than a preference.** CPack
# will happily produce an .rpm from a Debian build, and the result cannot be
# installed: rpmbuild derives the requirements from what the binaries actually
# link, and a Debian libcurl carries symbol versions -- `CURL_OPENSSL_4` --
# that no RPM distribution provides. `dnf install` refuses it outright, which is
# how this was found. So the package is built where it will be installed, and
# nothing about that is specific to curl: it is the general shape of packaging one
# family's binaries for another.
#
# The source is mounted read-only and the build happens inside the container, so
# nothing here touches the tree it is packaging. The .rpm is copied out and given
# back to the caller rather than left owned by root.
#
# Usage:
#   scripts/package-rpm.sh [output directory]
#
# **The image is a moving tag and must stay one.** An .rpm records the sonames its
# binaries link, and the container decides them: a package built on Fedora 40
# requires `libgit2.so.1.7()(64bit)` and `libarrow.so.1500()(64bit)`, and the
# current Fedora provides 1.9 and 2300. `dnf install` refuses it with "nothing
# provides", which is not a warning and not a downgrade -- the package does not
# install at all. So a pin to a release number is a pin to the day it was written,
# and it expires the moment that release does. See MOLE-389.
#
# MOLE_RPM_IMAGE overrides the distribution. Fedora because it is the family's
# reference and its Qt 6 packages are current; anything with cmake, a compiler and
# rpm-build will do.
#
set -uo pipefail

IMAGE="${MOLE_RPM_IMAGE:-fedora:latest}"
SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$SOURCE/build/packages}"

command -v docker >/dev/null 2>&1 || {
    echo "skipped: the .rpm is built in a container and docker is not installed" >&2
    echo "         (an .rpm built on this family cannot be installed on that one --" >&2
    echo "          see the note at the top of scripts/package-rpm.sh)" >&2
    exit 3
}

mkdir -p "$OUT"
# Absolute, because a bind mount is not a relative path: docker reads
# `build/packages` as the name of a volume and refuses it.
OUT="$(cd "$OUT" && pwd)"

echo "Building the .rpm in $IMAGE"

# One shell inside the container: install what the build needs, configure, build,
# pack, and hand the file back. Everything it installs is named here rather than in
# a Dockerfile, so what the package was built against is readable in one place.
docker run --rm \
    -v "$SOURCE:/src:ro" \
    -v "$OUT:/out" \
    -e CALLER="$(id -u):$(id -g)" \
    "$IMAGE" bash -c '
# pipefail as well as -e: a pipeline takes the status of its last command, so any
# check written as something | grep or something | tail cannot fail this script.
# TODO.md rule one. See MOLE-387.
set -eo pipefail
# Which release this turned out to be, on the record: `latest` is a moving tag, so
# the log is the only record of what the requirements were taken from.
. /etc/os-release
echo "  on Fedora $VERSION_ID"
dnf install -y -q gcc-c++ cmake ninja-build pkgconf-pkg-config rpm-build \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel qt6-qtsvg-devel \
    qt6-qtmultimedia-devel qt6-qtpdf-devel libarchive-devel libcurl-devel \
    openssl-devel libgit2-devel xxhash-devel libvterm-devel libsmbclient-devel \
    libnfs-devel >/dev/null

# Arrow where the distribution has it, so the package a family gets holds whatever
# that family can satisfy -- the same rule the .deb is built by, with a different
# answer: no Ubuntu archive has Arrow at any version, and Fedora packages it as
# libarrow-devel and parquet-libs-devel. Best-effort and never fatal, so a
# distribution that stops shipping it produces a package without the Parquet grid
# rather than no package at all. It says which way it went.
if dnf install -y -q libarrow-devel parquet-libs-devel >/dev/null 2>&1; then
    echo "  with the Parquet grid: this distribution packages Arrow"
else
    echo "  without the Parquet grid: this distribution does not package Arrow"
fi

# The tests are not built: the fast tier has already run on the machine that cut
# the release, and this build exists to be packaged.
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMOLE_BUILD_TESTS=OFF
cmake --build /build --parallel "$(nproc)"
cd /build && cpack -G RPM
cp /build/*.rpm /out/
chown "$CALLER" /out/*.rpm

# The sonames the package will ask its installer for, printed rather than left to
# be discovered by whoever runs dnf. This is the floor: every one of these has to
# be provided by the release the package is installed on, and a minor version in a
# soname means that release and not a later one.
#
# After the package has been handed over, so that nothing about reporting can lose
# one, and the grep cannot fail the script -- but an empty answer is said out loud,
# because a package that links nothing would mean something has gone very wrong.
# See MOLE-389.
linked=$(rpm -qp --requires /build/*.rpm | grep -E "\.so\." | sort || true)
echo "  it requires:"
if [ -n "$linked" ]; then
    printf "%s\n" "$linked" | sed "s/^/    /"
else
    echo "    nothing, which cannot be right for a package with binaries in it"
fi
'
status=$?
[ "$status" = 0 ] || {
    echo "the .rpm was not built" >&2
    exit "$status"
}

ls -1 "$OUT"/*.rpm
