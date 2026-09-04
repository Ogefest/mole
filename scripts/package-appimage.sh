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
# And what those bytes are, because a pinned version is not a pinned file: a
# release asset can be replaced under its tag, and this one is downloaded and then
# executed. Recorded rather than trusted, the way scripts/qt-tsan.sh keeps a table
# for the Qt source it compiles -- and enforced rather than reported, because 1.9.1
# is a tag that is finished and its bytes have no reason to change. Taken on
# 2026-09-04 from the URL above. See MOLE-390.
TOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"

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
    -e TOOL_SHA256="$TOOL_SHA256" \
    "$IMAGE" bash -c '
# pipefail as well as -e. Every check below ended in tee, tail or grep, whose
# status is the one the shell sees -- so a failing configure or a failing packer
# did not stop this script, and the failure surfaced two steps later as something
# else. TODO.md rule one: a check has to be able to fail. See MOLE-387.
set -eo pipefail

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
    >/tmp/configure.log 2>&1 || {
    cat /tmp/configure.log
    echo "configure failed" >&2
    exit 1
}
# The summary lines, out of the log. This was a pipeline ending in `grep … || true`,
# so a configure that printed its summaries and then died -- every FATAL_ERROR in
# src/app/CMakeLists.txt comes *after* them -- passed both this and the
# wanted/refused checks below, and failed later with an unrelated error.
# See MOLE-387.
#
# And through the script the three CI jobs use rather than a fourth hand-typed
# alternation: this one named ten of the eleven rows, in a pattern that mixed
# summaries with library names. A row that printed nothing fails here, which is
# what the script is for. See MOLE-390.
/src/scripts/configure-summary.sh /tmp/configure.log

# What this artefact has to have, from the one list all three consumers read.
# The exemptions -- no Parquet grid, and no Windows shares on purpose -- are named
# in that file rather than being the difference between two lists that drifted.
# See scripts/feature-summary.sh and MOLE-387.
. /src/scripts/feature-summary.sh
missing=0
mole_check_summary appimage /tmp/configure.log || missing=1
[ "$missing" = 0 ] || { echo "this AppImage would go out missing something the distribution has"; exit 1; }

cmake --build /build --parallel "$(nproc)"

# The same three steps `make bundle` takes, out of source because /src is read-only:
# install, strip, and bring the Qt libraries, plugins and QML modules in.
APPDIR=/appdir
cmake --install /build --prefix "$APPDIR/usr" >/dev/null
# Both binaries: naming `mole` alone left mole-tasks unstripped, which is 51 MB of
# debug symbols in an artefact people download. See MOLE-296.
# The stripping belongs to make-bundle.sh, over the plugin directories it
# discovers -- this had a path with lib/mole in it, which matches nothing on
# a lib64 distribution, which is every RPM one including the AlmaLinux this runs
# on. See MOLE-387.
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
# The bytes, before they are made executable and run. A pinned version is not a
# pinned file -- a release asset can be replaced under its tag -- and this one is
# the tool that packs an artefact somebody downloads.
got=$(sha256sum /tmp/appimagetool | cut -d" " -f1)
if [ "$got" != "$TOOL_SHA256" ]; then
    echo "appimagetool is not the file this script was written against:" >&2
    echo "  expected $TOOL_SHA256" >&2
    echo "  got      $got" >&2
    echo "Look at what changed under that tag before touching the line in" >&2
    echo "scripts/package-appimage.sh. See MOLE-390." >&2
    exit 1
fi
echo "  appimagetool sha256 matches the recorded one"
chmod +x /tmp/appimagetool

version=$(sed -n "s/^ *VERSION \([0-9][0-9.]*\)$/\1/p" /src/CMakeLists.txt)
# Extracted rather than mounted: appimagetool is itself an AppImage and a container
# has no FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
image="/out/mole-$version-x86_64.AppImage"
# The whole of the output on failure, the last lines on success: the packer says a
# great deal and only the tail is worth reading when it worked. Redirected to a
# file rather than piped, so the status this script sees belongs to the packer --
# with a pipe it belonged to tail, and a failing packer did not stop the script.
if ! ARCH=x86_64 /tmp/appimagetool "$APPDIR" "$image" >/tmp/appimagetool.log 2>&1; then
    cat /tmp/appimagetool.log
    echo "appimagetool failed" >&2
    exit 1
fi
tail -3 /tmp/appimagetool.log

# And there really is an image. The chown below was the only check against a
# packer that exits 0 having written nothing -- and it passes on a partial file.
[ -s "$image" ] || {
    echo "appimagetool produced no image at $image" >&2
    exit 1
}

chown "$CALLER" "$image"
'
status=$?
[ "$status" = 0 ] || {
    echo "the AppImage was not built" >&2
    exit "$status"
}

ls -1 "$OUT"/*.AppImage
