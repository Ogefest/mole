#!/usr/bin/env bash
#
# Turns an installed tree into a self-contained folder that runs on a machine
# with no Qt installed.
#
# Qt applications need three things beyond their own libraries: the platform
# plugin (or they abort with "could not load the Qt platform plugin"), the
# SQLite driver (or the index silently fails to open), and the QML modules the
# .qml files import. All three are found through environment variables rather
# than the linker, which is why the launcher script exists.
#
# Usage: scripts/make-bundle.sh <dist-dir>
set -euo pipefail

DIST="${1:?usage: make-bundle.sh <dist-dir>}"
BIN="$DIST/usr/bin/mole"
LIBDIR="$DIST/usr/lib"
# Mole's own plugins, wherever the install rules put them. Not assumed to be under
# lib: GNUInstallDirs gives lib64 on every RPM distribution, and with the scan below
# looking only under $LIBDIR their dependencies went uncollected -- so the AppImage's
# network plugin could not load for want of libsmbclient, reported as a problem
# nobody was reading. The archive plugin got away with it because libarchive came in
# as something else's dependency. See MOLE-296.
MOLE_PLUGIN_DIRS="$(find "$DIST/usr" -type d -path '*/mole/plugins' 2>/dev/null | tr '\n' ' ')"
PLUGINDIR="$DIST/usr/plugins"
QMLDIR="$DIST/usr/qml"

[[ -x "$BIN" ]] || { echo "no binary at $BIN" >&2; exit 1; }

QT_LIBDIR="$(dirname "$(ldd "$BIN" | awk '/libQt6Core/ {print $3}')")"
QT_ROOT="$QT_LIBDIR/qt6"
[[ -d "$QT_ROOT" ]] || QT_ROOT="$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || echo /usr/lib/qt6)"

mkdir -p "$LIBDIR" "$PLUGINDIR" "$QMLDIR"

# Libraries that must come from the host: they are tied to the running kernel,
# the graphics driver or the C library, and bundling them breaks more than it
# fixes.
is_excluded() {
    case "$1" in
        libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|ld-linux*|libresolv.so*)
            return 0 ;;
        libGL.so*|libGLX.so*|libEGL.so*|libGLdispatch.so*|libOpenGL.so*|libdrm.so*|libgbm.so*)
            return 0 ;;
        libX11*|libxcb*|libXau*|libXdmcp*|libXext*|libXrender*|libXi*|libXfixes*|libwayland*)
            return 0 ;;
        libgcc_s.so*|libstdc++.so*)
            return 0 ;;
        *) return 1 ;;
    esac
}

# Walks dependencies to a fixed point: a copied Qt library pulls in more Qt
# libraries, so one pass is never enough.
collect_deps() {
    local changed=1
    while (( changed )); do
        changed=0
        while IFS= read -r -d '' object; do
            while read -r name path; do
                [[ -n "$path" && -f "$path" ]] || continue
                is_excluded "$name" && continue
                [[ -f "$LIBDIR/$name" ]] && continue
                cp -L "$path" "$LIBDIR/$name"
                changed=1
            done < <(ldd "$object" 2>/dev/null | awk '/=> \//{print $1, $3}')
        done < <(find "$DIST/usr/bin" "$LIBDIR" "$PLUGINDIR" "$QMLDIR" $MOLE_PLUGIN_DIRS -type f \
                      \( -name '*.so*' -o -perm -u+x \) -print0 2>/dev/null)
    done
}

echo "  collecting Qt plugins"
for group in platforms platformthemes sqldrivers imageformats iconengines \
             xcbglintegrations wayland-shell-integration wayland-decoration-client \
             wayland-graphics-integration-client; do
    [[ -d "$QT_ROOT/plugins/$group" ]] || continue
    mkdir -p "$PLUGINDIR/$group"
    cp -a "$QT_ROOT/plugins/$group/." "$PLUGINDIR/$group/"
done

echo "  collecting QML modules"
if [[ -d "$QT_ROOT/qml" ]]; then
    cp -a "$QT_ROOT/qml/." "$QMLDIR/"
fi

echo "  resolving shared libraries"
collect_deps

echo "  stripping"
find "$LIBDIR" "$PLUGINDIR" "$QMLDIR" -name '*.so*' -type f -exec strip --strip-unneeded {} + 2>/dev/null || true

# The launcher is what makes the bundle relocatable. Qt looks these up at
# startup and there is no way to set them from inside main() early enough.
cat > "$DIST/mole" <<'LAUNCHER'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QML2_IMPORT_PATH="$HERE/usr/qml"
export QML_IMPORT_PATH="$HERE/usr/qml"
exec "$HERE/usr/bin/mole" "$@"
LAUNCHER
chmod +x "$DIST/mole"

echo ""
echo "  bundle: $DIST/mole"
# Apparent size, not disk usage: this machine's filesystem compresses, so `du -sh`
# reported 807K one run and 143M another for the same bundle. A figure nobody can
# reproduce is worse than none -- see MOLE-296.
echo "  size:   $(du -sh --apparent-size "$DIST" | cut -f1) (unpacked)"
