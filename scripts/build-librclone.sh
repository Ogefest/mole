#!/usr/bin/env bash
# Builds rclone as a shared library, so cloud and network drives are served
# in-process rather than by shelling out to the rclone binary or supervising a
# daemon.
#
# Needs the Go toolchain and about a gigabyte of module cache the first time.
# The result is roughly 115 MB, which is why it is built rather than vendored.
set -euo pipefail

VERSION="${RCLONE_VERSION:-v1.68.2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/third_party/librclone"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! command -v go >/dev/null 2>&1; then
    if [ -x /usr/local/go/bin/go ]; then
        export PATH="$PATH:/usr/local/go/bin"
    else
        echo "error: the Go toolchain is required to build librclone" >&2
        echo "       https://go.dev/dl/ , then run this again" >&2
        exit 1
    fi
fi

echo "Building librclone $VERSION (this takes a few minutes the first time)"

cd "$WORK"
cat > go.mod <<GOMOD
module molelibrclone

go 1.22
GOMOD

# A main package is required for -buildmode=c-shared; the exported C interface
# itself lives in rclone's own librclone package.
cat > main.go <<'MAIN'
package main

import _ "github.com/rclone/rclone/librclone"

func main() {}
MAIN

go get "github.com/rclone/rclone/librclone@$VERSION"
go mod tidy

mkdir -p "$OUT"
go build -buildmode=c-shared -o "$OUT/librclone.so" github.com/rclone/rclone/librclone

echo "  built $OUT/librclone.so"
echo "  reconfigure to pick it up:  cmake --preset debug"
