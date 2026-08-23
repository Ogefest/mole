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
dnf install -y -q gcc-c++ cmake ninja-build pkgconf-pkg-config file which findutils \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel qt6-qtsvg-devel \
    qt6-qtbase-gui qt6-qtmultimedia-devel libarchive-devel libcurl-devel openssl-devel \
    libgit2-devel xxhash-devel libsmbclient-devel >/dev/null

# What this distribution has not got is a decision by omission, so it is said out
# loud: no Arrow (so no Parquet grid), no Qt Pdf, no libvterm, no libnfs. The
# configure summary below is what a reader should be shown.
echo "--- what this build found ---"
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMOLE_BUILD_TESTS=OFF \
    2>&1 | grep -E "Parquet|Terminal|Git state|Network drives|Credential|Windows shares|NFS exports|Qt6 Pdf|Multimedia|libarchive" || true
cmake --build /build --parallel "$(nproc)"

# The same three steps `make bundle` takes, out of source because /src is read-only:
# install, strip, and bring the Qt libraries, plugins and QML modules in.
APPDIR=/appdir
cmake --install /build --prefix "$APPDIR/usr" >/dev/null
strip "$APPDIR/usr/bin/mole" "$APPDIR/usr/lib/mole/plugins/"*.so 2>/dev/null || true
/src/scripts/make-bundle.sh "$APPDIR"
cp /src/LICENSE /src/NOTICE /src/THIRD-PARTY-NOTICES.md "$APPDIR/"
cp -r /src/licenses "$APPDIR/"

# The licence check stays a hard failure. The LGPL conditions are not optional and
# a release is exactly when they matter.
#
# Run from inside the AppDir, because that is what the paperwork has to travel in:
# the check looks for the licence files in the working directory, so `make bundle`
# running it from the repository root asks whether *this repository* has them
# rather than whether the artefact does. Here it is asked about the artefact.
( cd "$APPDIR" && /src/scripts/licence-check.sh "$APPDIR/usr/bin/mole" )

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
