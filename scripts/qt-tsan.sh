#!/usr/bin/env bash
#
# Builds a ThreadSanitizer-instrumented qtbase, which is what makes `make tsan`
# mean anything.
#
# Qt annotates its own mutexes and futexes for ThreadSanitizer through
# QtCore/qtsan_impl.h, and every one of those annotations is behind a macro that
# is evaluated *when the translation unit is compiled*:
#
#   #if (__has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)) && …
#   #  define QT_BUILDING_UNDER_TSAN
#
# Our code is built with -fsanitize=thread and so gets the annotations from the
# header. A distribution's Qt is not, so the locking inside its libraries is
# invisible: TSan sees a write on one thread and a read on another with no
# happens-before between them and reports a race that is not there. Measured on
# this project, that was 5038 warnings, of which four in five were unattributable
# noise. Against the Qt this script builds: ten, all of them ours.
#
# qtbase only, deliberately. QtQuick, QtQml, QtPdf and QtMultimedia are not
# needed by any suite this tier runs -- and QtPdf drags in PDFium while
# QtMultimedia drags in a media stack, which is a great deal of building for
# suites that have nothing to do with concurrency. See ADR-0055.
#
# Usage:
#   scripts/qt-tsan.sh                 # build and install to the default prefix
#   MOLE_TSAN_QT=/somewhere scripts/qt-tsan.sh
#
set -euo pipefail

VERSION="${MOLE_TSAN_QT_VERSION:-6.4.2}"
PREFIX="${MOLE_TSAN_QT:-$HOME/opt/qt-$VERSION-tsan}"
WORK="${MOLE_TSAN_QT_WORKDIR:-${TMPDIR:-/tmp}/mole-qt-tsan}"
JOBS="${JOBS:-$(nproc)}"

# The published checksum for the version above. A source tarball fetched over the
# network and then compiled is exactly the thing to check rather than trust.
declare -A SHA256=(
    [6.4.2]=a88bc6cedbb34878a49a622baa79cace78cfbad4f95fdbd3656ddb21c705525d
)

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
die() { printf '\n%s\n' "$*" >&2; exit 1; }

branch="${VERSION%.*}"
tarball="qtbase-everywhere-src-$VERSION.tar.xz"
url="https://download.qt.io/archive/qt/$branch/$VERSION/submodules/$tarball"

if [ -x "$PREFIX/lib/libQt6Core.so.6" ] || [ -f "$PREFIX/lib/libQt6Core.so.6" ]; then
    if ldd "$PREFIX/lib/libQt6Core.so.6" | grep -q libtsan; then
        say "Already there"
        note "$PREFIX"
        note "libQt6Core.so.6 links libtsan, so this is the instrumented build."
        exit 0
    fi
    die "$PREFIX holds a Qt that is not instrumented. Remove it and run this again."
fi

command -v ninja >/dev/null || die "ninja is needed to build Qt."
command -v setarch >/dev/null || die "setarch is needed -- see the note below about ASLR."

mkdir -p "$WORK"
cd "$WORK"

say "Source"
if [ ! -f "$tarball" ]; then
    note "$url"
    curl -fsSL -o "$tarball.part" "$url" || die "could not download the Qt source"
    mv "$tarball.part" "$tarball"
fi
expected="${SHA256[$VERSION]:-}"
if [ -n "$expected" ]; then
    actual="$(sha256sum "$tarball" | cut -d' ' -f1)"
    [ "$actual" = "$expected" ] || die "checksum mismatch for $tarball:
  expected $expected
  got      $actual"
    note "sha256 matches the published one"
else
    note "no checksum recorded for $VERSION -- add one to this script"
fi

[ -d "qtbase-everywhere-src-$VERSION" ] || tar xf "$tarball"

say "Configure"
mkdir -p build
# -release with debug info rather than a debug build: what matters is the
# instrumentation, not the optimisation level, and a debug Qt is several times
# the size for stacks that are no more useful here.
(cd build && "../qtbase-everywhere-src-$VERSION/configure" \
    -prefix "$PREFIX" \
    -sanitize thread \
    -release -force-debug-info \
    -nomake examples -nomake tests \
    -opensource -confirm-license) || die "configure failed"

say "Build"
note "this takes a while -- qtbase is about 1500 translation units"
# Under setarch -R, and that is not optional.
#
# `-sanitize thread` instruments Qt's own build tools as well as its libraries,
# so the moc that runs during this build is itself a ThreadSanitizer binary --
# and it dies on "FATAL: ThreadSanitizer: unexpected memory mapping" because this
# distribution's mmap entropy is wider than TSan's shadow mapping expects.
# Without this the build fails at step 314 of 1557 with an error that says
# nothing about the cause. The Makefile's tsan target disables ASLR for the same
# reason when it runs the suites.
setarch "$(uname -m)" -R cmake --build build --parallel "$JOBS" || die "build failed"

say "Install"
setarch "$(uname -m)" -R cmake --install build || die "install failed"

ldd "$PREFIX/lib/libQt6Core.so.6" | grep -q libtsan \
    || die "installed, but libQt6Core.so.6 does not link libtsan -- the sanitizer did not take"

say "Ready"
note "$PREFIX"
note "make tsan finds it there by default; MOLE_TSAN_QT points somewhere else."
