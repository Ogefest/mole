#!/usr/bin/env bash
#
# The AppImage, built on the oldest distribution Mole intends to run on.
#
# **What it is built on is part of the artefact.** An AppImage carries its own Qt
# and its own libraries, but it links glibc dynamically and cannot carry that: a
# binary built against glibc 2.39 refuses to start on anything older with a
# `GLIBC_2.39 not found`, which to whoever downloaded it reads like a corrupt file.
# So the build image is a promise rather than a build detail, and it is written
# down: here, in TODO.md, and in the release notes.
#
# **The floor is glibc 2.34, which is AlmaLinux 9.** It was chosen for what it
# reaches rather than for itself: 2.34 is older than Ubuntu 22.04's 2.35, Debian
# 12's 2.36 and Ubuntu 24.04's 2.39, so one artefact covers every distribution from
# 2021 onwards. It is also the oldest image that can build Mole at all -- Qt 6.4 is
# the baseline and EPEL 9 has 6.6, while Ubuntu 22.04's own archive stops at 6.2
# and Debian 12 has 6.4 with a newer glibc than Alma. Raising the floor is a
# decision about who can run this; lowering it means finding a Qt 6.4 for an older
# distribution, which is a bigger piece of work than this ticket.
#
# Usage:
#   scripts/package-appimage.sh [output directory]
#
# MOLE_APPIMAGE_BASE overrides the build image, and doing so changes the floor.
#
set -uo pipefail

IMAGE="${MOLE_APPIMAGE_BASE:-almalinux:9}"
SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$SOURCE/build/packages}"
# Pinned rather than `continuous`: the tool that packs a release is part of the
# release, and "whatever was built this morning" is not something to publish
# against. From AppImage/appimagetool, which is where the tool lives now -- the old
# AppImageKit assets were renamed `obsolete-*`, so the URL every guide still gives
# is a 404.
TOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"

command -v docker >/dev/null 2>&1 || {
    echo "skipped: the AppImage is built on an older distribution, in a container," >&2
    echo "         and docker is not installed -- see the note in this script" >&2
    exit 3
}

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

echo "Building the AppImage on $IMAGE"

docker run --rm \
    -v "$SOURCE:/src:ro" \
    -v "$OUT:/out" \
    -e CALLER="$(id -u):$(id -g)" \
    -e TOOL_URL="$TOOL_URL" \
    "$IMAGE" bash -c '
set -e

# EPEL for Qt 6 and CRB for what its devel packages need. Named here rather than in
# a Dockerfile so that what the artefact was built against is readable in one place.
dnf install -y -q dnf-plugins-core >/dev/null
dnf config-manager --set-enabled crb >/dev/null
dnf install -y -q epel-release >/dev/null
# xcb-util-cursor is not a build dependency and is in this list anyway: the Qt xcb
# platform plugin has hard-required it since 6.5, nothing else pulls it in, and
# make-bundle.sh can only bundle what the machine it runs on has. Without it the
# AppImage aborts on start with "no Qt platform plugin could be initialized" on
# every machine that has not got it -- which is most of them. See MOLE-300.
# (No apostrophes in this block: it is inside a single-quoted container script.)
#
# Everything EPEL 9 has, which is everything: the first version of this list was
# short, and the AppImage went out without the Parquet grid, without PDF rendering,
# with a reduced terminal parser and with no NFS drives -- none of which was a
# property of the distribution. Checked package by package rather than assumed, and
# the step below refuses a build that came out missing one.
dnf install -y -q gcc-c++ cmake ninja-build pkgconf-pkg-config file which findutils \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel qt6-qtsvg-devel \
    qt6-qtbase-gui qt6-qtmultimedia-devel qt6-qtpdf-devel libarchive-devel \
    libcurl-devel openssl-devel libgit2-devel xxhash-devel libsmbclient-devel \
    libnfs-devel libvterm-devel libarrow-devel parquet-libs-devel \
    xcb-util-cursor >/dev/null

# What the build found, printed and then held to. An artefact that quietly came out
# without a feature is the fault MOLE-120 built its own check for; this is the same
# question asked on the oldest distribution.
echo "--- what this build found ---"
#
# MOLE_WITH_SMB is off here and nowhere else. libsmbclient is GPL-3.0-or-later and
# an AppImage is one artefact somebody is handed; the .deb and the .rpm keep
# Windows shares, because there the library comes from the distribution. The same
# call MOLE-322 made about the video codecs a distribution ships. See ADR-0094.
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMOLE_BUILD_TESTS=OFF \
    -DMOLE_WITH_SMB=OFF \
    2>&1 | tee /tmp/configure.log \
    | grep -E "Parquet|Terminal|Git state|Network drives|Credential|Windows shares|NFS exports|Qt6 Pdf|Multimedia|libarchive" || true

# Everything the distribution can give, which is everything except the Parquet
# grid: EPEL 9 ships Arrow 9.0.0 and no ParquetConfig.cmake at all, so
# find_package(Parquet) cannot succeed there whatever is installed. That one
# absence is a property of the oldest distribution Mole runs on, and it is written
# in TODO.md and in the release notes rather than discovered by a downloader.
missing=0
for wanted in "Terminal: libvterm" "Git state: libgit2" \
    "Credential store: OpenSSL" "Network drives: sftp, ftp, s3, webdav" \
    "NFS exports: nfs"; do
    grep -qF "$wanted" /tmp/configure.log || { echo "missing: $wanted"; missing=1; }
done
# The one feature this artefact is deliberately without, asserted as an absence:
# libsmbclient-devel is installed above, so a build that stopped passing
# -DMOLE_WITH_SMB=OFF would silently find it and go out carrying GPL-3 code.
grep -qF "Windows shares: not built" /tmp/configure.log \
    || { echo "this AppImage was built with Windows shares, which is GPL-3 in an Apache-2.0 artefact"; missing=1; }
for refused in "Qt6 Pdf not found" "Qt6 Multimedia not found" "libarchive not found"; do
    grep -qF "$refused" /tmp/configure.log && { echo "not built with: $refused"; missing=1; }
done
[ "$missing" = 0 ] || { echo "this AppImage would go out missing something the distribution has"; exit 1; }

cmake --build /build --parallel "$(nproc)"

# The same three steps `make bundle` takes, out of source because /src is read-only:
# install, strip, and bring the Qt libraries, plugins and QML modules in.
APPDIR=/appdir
cmake --install /build --prefix "$APPDIR/usr" >/dev/null
# Both binaries: naming `mole` alone left mole-tasks unstripped, which is 51 MB of
# debug symbols in an artefact people download. See MOLE-296.
strip "$APPDIR/usr/bin/"* "$APPDIR/usr/lib/mole/plugins/"*.so 2>/dev/null || true
/src/scripts/make-bundle.sh "$APPDIR"
cp /src/LICENSE /src/NOTICE /src/THIRD-PARTY-NOTICES.md "$APPDIR/"
cp -r /src/licenses "$APPDIR/"

# The licence check stays a hard failure. The LGPL conditions are not optional and
# a release is exactly when they matter.
#
# The artefact root is given rather than implied by the working directory, which is
# what MOLE-298 separated: the paperwork question is about the AppDir, and the
# question about which Qt modules the build uses is about /src. Before that, this
# call worked by being run from inside the AppDir and the source question went
# unanswered.
/src/scripts/licence-check.sh "$APPDIR/usr/bin/mole" "$APPDIR"

# What the format wants and the bundle does not already arrange: an AppRun, and the
# desktop entry and icon at the top level, where a desktop that integrates
# AppImages looks for them. The launcher make-bundle.sh wrote is the AppRun -- it
# already points Qt at the bundled plugins and QML modules.
mv "$APPDIR/mole" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp /src/packaging/mole.desktop "$APPDIR/mole.desktop"
cp /src/packaging/mole.svg "$APPDIR/mole.svg"

curl -sSfL -o /tmp/appimagetool "$TOOL_URL"
chmod +x /tmp/appimagetool

version=$(sed -n "s/^ *VERSION \([0-9][0-9.]*\)$/\1/p" /src/CMakeLists.txt)
# Extracted rather than mounted: appimagetool is itself an AppImage and a container
# has no FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
ARCH=x86_64 /tmp/appimagetool "$APPDIR" "/out/mole-$version-x86_64.AppImage" 2>&1 | tail -3

chown "$CALLER" /out/*.AppImage
'
status=$?
[ "$status" = 0 ] || {
    echo "the AppImage was not built" >&2
    exit "$status"
}

ls -1 "$OUT"/*.AppImage
