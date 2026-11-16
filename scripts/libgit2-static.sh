#!/usr/bin/env bash
#
# libgit2, built from a pinned release as a static library, for the .deb.
#
# **Why the .deb carries its own.** libgit2 changes its soname with every minor
# version, because its ABI changes with them: Ubuntu 24.04 has libgit2-1.7, Debian
# 13 and Ubuntu 26.04 have libgit2-1.9, and a binary linked to `libgit2.so.1.7`
# cannot load 1.9. No version exists on all three distributions the one .deb is for,
# so a .deb that names a libgit2 package installs on one of them at most -- which
# is what 0.1.4 did. Linked statically, git state needs nothing from the archive and
# `libgit2-…` leaves the Depends line. See MOLE-458.
#
# **Every network transport is off.** Mole only reads the state of local
# repositories (ADR-0041), so neither SSH nor HTTPS is needed, and turning them off
# is also what keeps libssh2, OpenSSL and GSSAPI out of the link. The regular
# expressions, the HTTP parser, zlib and the SHA implementations are libgit2's own
# bundled copies, so the static library adds no runtime dependency of its own.
#
# **Carrying the library makes its security fixes Mole's to pick up.** The pin is
# below and nowhere else: bumping it is changing LIBGIT2_VERSION and LIBGIT2_SHA256
# together, from the tag archive GitHub serves for the new release. Look at
# https://github.com/libgit2/libgit2/releases when cutting a release.
#
# Usage:
#   scripts/libgit2-static.sh <prefix>
#
# Leaves `<prefix>/lib/libgit2.a`, its headers and its pkg-config file. Built once
# per pin: a prefix already holding this version is left as it is.
#
set -euo pipefail

# 1.9.7, the newest 1.9 release on 2026-11-16 -- 2.0 was a release candidate. The
# digest is of the bytes GitHub served for the tag archive that day.
LIBGIT2_VERSION="1.9.7"
LIBGIT2_SHA256="1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7"
LIBGIT2_URL="https://github.com/libgit2/libgit2/archive/refs/tags/v$LIBGIT2_VERSION.tar.gz"

[ $# = 1 ] || { echo "usage: $0 <prefix>" >&2; exit 2; }
mkdir -p "$1"
PREFIX="$(cd "$1" && pwd)"
STAMP="$PREFIX/.libgit2-$LIBGIT2_VERSION-$LIBGIT2_SHA256"

if [ -f "$STAMP" ] && [ -f "$PREFIX/lib/libgit2.a" ]; then
    echo "  libgit2 $LIBGIT2_VERSION (static) is already at $PREFIX"
    exit 0
fi

WORK="$PREFIX/.work"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# Retried, and a server error counts -- the reason is in package-appimage.sh: GitHub's
# archive hosting answering 504 has stopped a release rehearsal before.
curl -sSfL --retry 8 --retry-all-errors --connect-timeout 30 \
    -o "$WORK/libgit2.tar.gz" "$LIBGIT2_URL"
got=$(sha256sum "$WORK/libgit2.tar.gz" | cut -d" " -f1)
if [ "$got" != "$LIBGIT2_SHA256" ]; then
    echo "libgit2 $LIBGIT2_VERSION: expected sha256 $LIBGIT2_SHA256, got $got -- refusing to build it" >&2
    exit 1
fi
tar -xzf "$WORK/libgit2.tar.gz" -C "$WORK"

# Static and position-independent, because it is linked into mole_core, which is
# linked into the plugins as well as the application.
cmake -S "$WORK/libgit2-$LIBGIT2_VERSION" -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=OFF -DBUILD_CLI=OFF -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF \
    -DUSE_SSH=OFF -DUSE_HTTPS=OFF -DUSE_GSSAPI=OFF -DUSE_NTLMCLIENT=OFF \
    -DUSE_SHA1=CollisionDetection -DUSE_SHA256=Builtin \
    -DUSE_HTTP_PARSER=builtin -DREGEX_BACKEND=builtin -DUSE_BUNDLED_ZLIB=ON \
    >"$WORK/configure.log" 2>&1 || { cat "$WORK/configure.log"; exit 1; }
cmake --build "$WORK/build" >"$WORK/build.log" 2>&1 || { tail -50 "$WORK/build.log"; exit 1; }
cmake --install "$WORK/build" >/dev/null

# No shared library may be beside it, or the linker prefers it and the point is lost.
shared=$(find "$PREFIX/lib" -name 'libgit2.so*')
if [ -n "$shared" ]; then
    echo "libgit2 $LIBGIT2_VERSION installed a shared library at $PREFIX/lib -- refusing" >&2
    exit 1
fi

rm -f "$PREFIX"/.libgit2-*
touch "$STAMP"
echo "  libgit2 $LIBGIT2_VERSION (static, no network transports) at $PREFIX"
