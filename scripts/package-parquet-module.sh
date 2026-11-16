#!/usr/bin/env bash
#
# The .deb's Parquet module, built in an ubuntu:26.04 container.
#
# **Why there.** The one .deb is built on Ubuntu 24.04 and installs on Debian 13,
# Ubuntu 24.04 and Ubuntu 26.04, and 26.04 is the only one of the three whose
# archive has Apache Arrow (libarrow2300, libparquet2300). So the Arrow half of the
# Parquet preview is built where Arrow is, as `mole-parquet.so`, and `make deb` puts
# it into the package built on 24.04. On 26.04 it loads against the distribution's
# Arrow and Qt 6.10; on the other two it does not load, and the preview falls back
# to the file-information view. See ADR-0106 and MOLE-459.
#
# Built against Qt 6.10 here, and that is right rather than a mismatch to manage:
# 26.04 is the only system where it can load, and 26.04's Qt is 6.10. Where it
# cannot load, ParquetModule says why -- the module was built for a newer system --
# rather than crashing.
#
# What it leaves in the output directory:
#
#   mole-parquet.so          the module, stripped
#   mole-parquet.recommends  one package per line: what the module links, as 26.04
#                            names it. Derived from the module's NEEDED entries with
#                            dpkg -S, never typed, so the next Arrow soname follows
#                            by itself. packaging/deb-parquet-module.cmake turns the
#                            ones the .deb does not already depend on into Recommends.
#
# Usage:
#   scripts/package-parquet-module.sh <output directory>
#
# Exits 3 when docker is not installed, which `make deb` reads as "skipped" and
# builds the .deb without the module -- the same rule as scripts/package-rpm.sh.
# MOLE_PARQUET_MODULE_IMAGE overrides the image.
#
set -uo pipefail

IMAGE="${MOLE_PARQUET_MODULE_IMAGE:-ubuntu:26.04}"
SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?usage: package-parquet-module.sh <output directory>}"

# Cleared before anything else, the skip included: a module left by an earlier run
# would otherwise be packed by `make deb` as if this run had built it.
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT/mole-parquet.so" "$OUT/mole-parquet.recommends"

command -v docker >/dev/null 2>&1 || {
    echo "skipped: the Parquet module is built in an $IMAGE container and docker is not installed" >&2
    exit 3
}

echo "Building the Parquet module in $IMAGE"

docker run --rm \
    -v "$SOURCE:/src:ro" \
    -v "$OUT:/out" \
    -e CALLER="$(id -u):$(id -g)" \
    "$IMAGE" bash -c '
set -eo pipefail
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
echo "  on $PRETTY_NAME"
apt-get update -qq >/dev/null
# Only what the module needs: a compiler, qtbase for the headers the interface
# uses, and Arrow. A core-only configure asks for nothing else.
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config file dpkg-dev binutils \
    qt6-base-dev libarrow-dev libparquet-dev >/dev/null

cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMOLE_CORE_ONLY=ON -DMOLE_BUILD_TESTS=OFF > /tmp/configure.log 2>&1 \
    || { cat /tmp/configure.log; exit 1; }
sed -n "s/^-- \(Parquet preview: .*\)/  \1/p" /tmp/configure.log
cmake --build /build --target mole_parquet > /tmp/build.log 2>&1 \
    || { tail -40 /tmp/build.log; exit 1; }
module=/build/modules/mole-parquet.so
[ -f "$module" ] || { echo "the module was not built -- is Arrow installed in this image?" >&2; exit 1; }
strip --strip-unneeded "$module"

# What it links, as packages of this distribution.
: > /tmp/recommends
for soname in $(objdump -p "$module" | sed -n "s/^ *NEEDED *//p"); do
    # Every match, and then the first: a `| head -1` here would close the pipe on
    # sed, and under pipefail a SIGPIPE ends the whole script.
    paths=$(ldconfig -p | sed -n "s|^[[:space:]]*$soname (libc6,x86-64[^)]*) => ||p")
    read -r path <<< "$paths" || true
    [ -n "$path" ] || { echo "nothing on this system provides $soname" >&2; exit 1; }
    # The file the soname points at, and the soname itself, because which of the
    # two dpkg registered depends on whether the package ships /lib or /usr/lib.
    owner=$(dpkg -S "$(readlink -f "$path")" 2>/dev/null || dpkg -S "$path" 2>/dev/null || true)
    package=${owner%%:*}
    [ -n "$package" ] || { echo "no package owns $path" >&2; exit 1; }
    echo "$package" >> /tmp/recommends
done
sort -u /tmp/recommends > /out/mole-parquet.recommends
cp "$module" /out/mole-parquet.so
chown "$CALLER" /out/mole-parquet.so /out/mole-parquet.recommends
echo "  it links (as packages): $(tr "\n" " " < /out/mole-parquet.recommends)"
'
status=$?
[ "$status" = 0 ] || {
    echo "the Parquet module was not built" >&2
    exit "$status"
}
ls -1 "$OUT/mole-parquet.so"
