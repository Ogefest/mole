#!/usr/bin/env bash
#
# Verifies the conditions that let an Apache-2.0 application use LGPL Qt.
# See docs/LICENSING.md for the reasoning; this is the enforcement.
#
# **Two kinds of question, and telling them apart is the whole of MOLE-298.**
#
# About the *source tree* -- which Qt modules the build uses. Asked of the
# repository this script lives in, found from its own path, so the answer cannot
# depend on where it was run from.
#
# About an *artefact* -- the paperwork, and whether a bundled Qt stays replaceable.
# Asked of a root: given, or derived from the binary. It used to be asked of the
# working directory, which meant `make bundle` running it from the repository root
# was asking whether *this repository* carries LICENSE. It always does, so the
# guard could not fail for the reason it exists, and a `dist/` carrying none of the
# five files passed. Nothing published was wrong -- `make bundle` copies them
# itself -- but a check that is green because of where it ran is not a check.
#
# Usage:
#   scripts/licence-check.sh                     every artefact it can find, each named
#   scripts/licence-check.sh <binary> [<root>]   one artefact
#
set -uo pipefail

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail=0
ok() { printf '  \033[32mOK\033[0m    %s\n' "$1"; }
bad() {
    printf '  \033[31mFAIL\033[0m  %s\n' "$1"
    fail=1
}
# Neither a pass nor a failure: a question that does not apply to this artefact.
# Printed rather than skipped in silence, because "not asked" and "asked and
# answered" are what this whole ticket is about.
note() { printf '  \033[33m--\033[0m    %s\n' "$1"; }

# ---------------------------------------------------------------- the source tree

# 3. Only LGPL Qt modules. Charts, DataVisualization, VirtualKeyboard and friends
#    are GPL-or-commercial and would change the answer entirely.
#
#    Asked of $SOURCE and not of `.`: run from inside an AppDir -- which is how the
#    AppImage invokes this -- a search of the working directory finds no
#    CMakeLists.txt at all and reports no GPL-only modules, which is a pass by
#    absence.
check_source_tree() {
    if [[ ! -f "$SOURCE/CMakeLists.txt" ]]; then
        bad "cannot find the source tree at $SOURCE, so which Qt modules the build uses is unanswered"
        return
    fi
    local gpl_only
    gpl_only=$(grep -rhoE 'Qt6::(Charts|DataVisualization|VirtualKeyboard|Lottie|Quick3DAssetImport)' \
        --include=CMakeLists.txt "$SOURCE" | sort -u)
    if [[ -z "$gpl_only" ]]; then
        ok "no GPL-only Qt modules are used"
    else
        bad "GPL-only Qt module in use: $gpl_only"
    fi
}

# ------------------------------------------------------------------- the binary

check_binary() {
    local bin="$1"

    # 1. Qt must be dynamically linked. Static linking would drag the application
    #    itself under LGPL relinking obligations.
    local qt_shared
    qt_shared=$(ldd "$bin" 2>/dev/null | grep -c 'libQt6')
    if ((qt_shared > 0)); then
        ok "Qt is dynamically linked ($qt_shared libraries)"
    else
        bad "no dynamically linked Qt found -- is it statically linked?"
    fi

    # 2. Qt symbols must live in the shared libraries, not inside our binary.
    if grep -qE ' T (QQuickItem|QQmlEngine|QCoreApplication)::' \
            <<<"$(nm -C --defined-only "$bin" 2>/dev/null)"; then
        bad "Qt symbols are defined inside the binary -- Qt appears to be static"
    else
        ok "no Qt symbols compiled into the binary"
    fi
}

# ----------------------------------------------------------------- the artefact

# Where an artefact keeps its paperwork. Two layouts, because there are two kinds
# of artefact: a bundle or an AppDir carries it at the top, and an installed tree
# -- which is what a .deb or an .rpm unpacks to -- carries it where the install
# rules put it.
docdir_for() {
    local root="$1"
    if [[ -f "$root/LICENSE" ]]; then
        printf '%s' "$root"
    elif [[ -f "$root/usr/share/doc/mole/LICENSE" ]]; then
        printf '%s' "$root/usr/share/doc/mole"
    else
        return 1
    fi
}

root_for() {
    local bin="$1"
    case "$bin" in
        */usr/bin/*) printf '%s' "${bin%/usr/bin/*}" ;;
        *) dirname "$bin" ;;
    esac
}

# 4. The paperwork has to travel with the build.
check_paperwork() {
    local root="$1" docdir
    if ! docdir=$(docdir_for "$root"); then
        bad "no licence paperwork under $root (looked at $root/ and $root/usr/share/doc/mole/)"
        return
    fi
    local f
    for f in LICENSE NOTICE THIRD-PARTY-NOTICES.md licenses/LGPL-3.0.txt licenses/Apache-2.0.txt; do
        [[ -f "$docdir/$f" ]] && ok "present: $docdir/$f" || bad "missing: $docdir/$f"
    done
}

# 5. In a bundle, Qt has to remain replaceable. A distribution package has no
#    bundled Qt at all, which is a different answer rather than a failure.
#
#    The stale-bundle case is named explicitly rather than reported as a bare
#    failure: this check once fired on a dist/ left over from before the project was
#    renamed -- the launcher in it was still called superfilemanager -- and "bundled
#    Qt cannot be replaced" is a puzzling way to say "that is not this project's
#    bundle".
check_replaceable() {
    local root="$1" bin="$2" launcher
    if [[ ! -e "$root/usr/lib/libQt6Core.so.6" ]]; then
        note "no bundled Qt under $root/usr/lib, so there is nothing to keep replaceable"
        return
    fi
    launcher="$root/AppRun"
    [[ -f "$launcher" ]] || launcher="$root/$(basename "$bin")"
    if [[ ! -f "$launcher" ]]; then
        bad "$root holds a bundle but there is no launcher at $launcher (stale bundle? run: make bundle)"
    elif [[ ! -w "$root/usr/lib/libQt6Core.so.6" ]]; then
        bad "bundled Qt at $root/usr/lib is not writable, so the user cannot replace it"
    elif ! grep -q LD_LIBRARY_PATH "$launcher"; then
        bad "$launcher does not set LD_LIBRARY_PATH, so a replaced Qt would not be found"
    else
        ok "bundled Qt is writable and found via LD_LIBRARY_PATH (replaceable)"
    fi
}

check_artefact() {
    local bin="$1" root="${2:-}"
    [[ -n "$root" ]] || root=$(root_for "$bin")

    echo "Licence check: $bin (artefact root: $root)"
    if [[ ! -x "$bin" ]]; then
        bad "no binary at $bin"
        return
    fi
    check_binary "$bin"
    check_source_tree
    check_paperwork "$root"
    check_replaceable "$root" "$bin"
}

# --------------------------------------------------------------- every artefact

SCRATCH=""
cleanup() { [[ -z "$SCRATCH" ]] || rm -rf "$SCRATCH"; }
trap cleanup EXIT

# Each artefact the release publishes, asked the same question -- or named as
# unasked, with the reason. A compliance check with a silent gap in its coverage is
# the same fault as one that reports on the wrong directory.
sweep() {
    local found=0
    SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mole-licence.XXXXXX")

    if [[ -d "$SOURCE/dist" ]]; then
        found=1
        check_artefact "$SOURCE/dist/usr/bin/mole" "$SOURCE/dist"
        echo
    fi

    local deb
    for deb in "$SOURCE"/build/packages/*.deb; do
        [[ -f "$deb" ]] || continue
        found=1
        if ! command -v dpkg-deb >/dev/null 2>&1; then
            echo "Licence check: $(basename "$deb")"
            note "not asked: no dpkg-deb on this machine to unpack it with"
            echo
            continue
        fi
        rm -rf "$SCRATCH/deb" && mkdir -p "$SCRATCH/deb"
        dpkg-deb -x "$deb" "$SCRATCH/deb"
        echo "Unpacked $(basename "$deb")"
        check_artefact "$SCRATCH/deb/usr/bin/mole" "$SCRATCH/deb"
        echo
    done

    local image
    for image in "$SOURCE"/build/packages/*.AppImage; do
        [[ -f "$image" ]] || continue
        found=1
        rm -rf "$SCRATCH/app" && mkdir -p "$SCRATCH/app"
        # --appimage-extract needs no FUSE, which is the point: this has to work
        # wherever a release is checked, including inside a container.
        ( cd "$SCRATCH/app" && "$image" --appimage-extract >/dev/null 2>&1 )
        if [[ -d "$SCRATCH/app/squashfs-root" ]]; then
            echo "Unpacked $(basename "$image")"
            check_artefact "$SCRATCH/app/squashfs-root/usr/bin/mole" "$SCRATCH/app/squashfs-root"
        else
            echo "Licence check: $(basename "$image")"
            bad "could not unpack it, so nothing about it was checked"
        fi
        echo
    done

    local rpm
    for rpm in "$SOURCE"/build/packages/*.rpm; do
        [[ -f "$rpm" ]] || continue
        found=1
        echo "Licence check: $(basename "$rpm")"
        if command -v rpm2cpio >/dev/null 2>&1 && command -v cpio >/dev/null 2>&1; then
            rm -rf "$SCRATCH/rpm" && mkdir -p "$SCRATCH/rpm"
            ( cd "$SCRATCH/rpm" && rpm2cpio "$rpm" | cpio -idm --quiet )
            check_artefact "$SCRATCH/rpm/usr/bin/mole" "$SCRATCH/rpm"
        else
            # Named rather than skipped: this is the one artefact a Debian machine
            # cannot unpack, and the release workflow asks it inside the Fedora
            # container that installs it.
            note "not asked here: no rpm2cpio on this machine. The release workflow"
            note "asks it inside the container that installs the .rpm."
        fi
        echo
    done

    if [[ "$found" = 0 ]]; then
        # Not a pass. There is nothing to be compliant about.
        bad "no artefacts found under dist/ or build/packages/ -- run make bundle or make packages first"
    fi
}

# ------------------------------------------------------------------ dispatch

if [[ $# -eq 0 ]]; then
    sweep
else
    check_artefact "$1" "${2:-}"
fi

echo
if ((fail)); then
    echo "Not compliant. See docs/LICENSING.md."
    exit 1
fi
echo "Compliant."
