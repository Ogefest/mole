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

# Libraries that must come from the host, and the test for being one.
#
# **The test is whether it is tied to the host's kernel, graphics driver, display
# server or C library** -- not whether its name looks like one that is. That
# distinction was implicit and a pattern got it wrong: `libxcb*` also matches
# xcb-util-cursor, which is an ordinary helper library that Qt's xcb plugin has
# hard-required since 6.5 and that nothing on a desktop installs by itself. So the
# AppImage carried a platform plugin it could not load, and aborted on start on any
# machine without libxcb-cursor0 -- including the one this project is developed on.
# See MOLE-300.
#
# Anything not answering that test is bundled, because a library that is merely
# usually present is a library that is sometimes not.
is_excluded() {
    case "$1" in
        # The xcb-util family. Listed first, and it is the whole point of the list
        # having a reason: these are userspace helpers over the protocol library,
        # coupled to nothing, and each is a separate package that only arrives when
        # something asks for it.
        libxcb-cursor.so* | libxcb-util.so* | libxcb-image.so* | libxcb-keysyms.so*             | libxcb-icccm.so* | libxcb-render-util.so*)
            return 1 ;;

        # The C library and its loader. Bundling these is how an AppImage stops
        # working on the distribution it was built for.
        libc.so* | libm.so* | libdl.so* | librt.so* | libpthread.so* | ld-linux* | libresolv.so*)
            return 0 ;;

        # The graphics driver's own stack: what these load is chosen by the kernel
        # module and the card in the machine, not by us.
        libGL.so* | libGLX.so* | libEGL.so* | libGLdispatch.so* | libOpenGL.so* | libdrm.so* | libgbm.so*)
            return 0 ;;

        # The display server's client side. libX11 and libxcb are one half of a
        # protocol whose other half is running on the machine, and every X or
        # Wayland desktop has them because nothing draws a window without them.
        libX11.so* | libX11-xcb.so* | libxcb.so* | libXau.so* | libXdmcp.so* | libwayland*)
            return 0 ;;

        # X extensions, on the same terms: they arrive with the client stack any
        # desktop already has, and each pairs with a server-side extension.
        #
        # **Named, not matched.** This group used to end in `libxcb-*.so*`, and that
        # is how libxcb-cursor came to be excluded -- by resemblance to libraries it
        # has nothing in common with. A name here is a judgement that every target
        # has it; a pattern is a judgement about everything that will ever look like
        # it, which is not a judgement anybody made. Anything not on this list is
        # bundled if the build machine has it and fails the check below if it has
        # not, so the next xcb library Qt requires is decided rather than assumed.
        # See MOLE-302.
        libXext.so* | libXrender.so* | libXi.so* | libXfixes.so* \
            | libxcb-randr.so* | libxcb-render.so* | libxcb-shape.so* | libxcb-shm.so* \
            | libxcb-sync.so* | libxcb-xfixes.so* | libxcb-xkb.so* | libxcb-glx.so* \
            | libxcb-present.so* | libxcb-dri2.so* | libxcb-dri3.so* | libxcb-xinerama.so*)
            return 0 ;;

        # The C++ runtime. Left to the host because the host's is never older than
        # the one this was built against -- the AppImage's floor is the oldest
        # distribution Mole runs on, and the tarball's build host is newer still.
        libgcc_s.so* | libstdc++.so*)
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

# A Qt plugin whose libraries this machine cannot provide is left out rather than
# carried. Qt loads what it finds and shrugs at what will not load, so an unloadable
# plugin is either harmless -- a platform theme, a style -- or fatal, which is the
# platform plugin itself and is checked separately below. Carrying one that cannot
# load is weight and a puzzle: the AppImage was shipping libqgtk3.so with no GTK on
# the build host to bundle. See MOLE-300.
while IFS= read -r -d '' plugin; do
    short_of=$(LD_LIBRARY_PATH="$LIBDIR" ldd "$plugin" 2>/dev/null | awk '/not found/{print $1}' | tr '\n' ' ')
    [[ -n "$short_of" ]] || continue
    echo "  leaving out ${plugin#"$PLUGINDIR"/}: this machine has no ${short_of% }"
    rm -f "$plugin"
done < <(find "$PLUGINDIR" -name '*.so' -print0 2>/dev/null)

# The one plugin that is not optional: without a platform plugin the application
# aborts on start rather than falling back to anything.
if [[ ! -e "$PLUGINDIR/platforms/libqxcb.so" ]]; then
    echo "  the xcb platform plugin is not in the bundle, so it would not start at all"
    exit 1
fi

# Nothing in the bundle may be short of a library. `ldd` over every object it holds,
# with the bundle's own lib directory ahead of the host's, and anything still "not
# found" is a dependency this machine did not have to give -- so it was skipped in
# silence, which is how the AppImage went out carrying a platform plugin it could
# not load. A bundle that is missing something is not a bundle. See MOLE-300.
echo "  checking the bundle is complete"
# Every library every object asks for, judged against the two places it is allowed
# to come from: inside the bundle, or named by is_excluded as the host's.
#
# **Not against whether it resolves.** `ldd` searches the bundle and then the host,
# so a library excluded by mistake that the build machine happens to have resolves,
# reports found, and passes -- while the artefact goes out without it. That is
# MOLE-300 in general form: the fault there was caught only because this workstation
# was also missing libxcb-cursor0, and had the build host had it the check would have
# been green and the AppImage would still have aborted everywhere else. The soname is
# what is asked about, which is what is_excluded already decides with. See MOLE-302.
short=0
while IFS= read -r -d '' object; do
    while read -r name; do
        [[ -e "$LIBDIR/$name" ]] && continue     # in the bundle
        is_excluded "$name" && continue          # deliberately the host's
        echo "  missing: $name, needed by ${object#"$DIST"/}"
        short=1
    done < <(objdump -p "$object" 2>/dev/null | awk '/NEEDED/{print $2}')
done < <(find "$DIST/usr/bin" "$LIBDIR" "$PLUGINDIR" "$QMLDIR" $MOLE_PLUGIN_DIRS -type f \
              \( -name '*.so*' -o -perm -u+x \) -print0 2>/dev/null)
if [[ "$short" != 0 ]]; then
    echo ""
    echo "  This machine has not got everything the bundle needs, so the bundle would"
    echo "  start on this machine and fail on one without those libraries. Install them"
    echo "  and run again -- libxcb-cursor0 (Debian/Ubuntu) or xcb-util-cursor (RPM) is"
    echo "  the usual one: Qt's xcb platform plugin has required it since 6.5 and"
    echo "  nothing on a desktop installs it by itself."
    exit 1
fi

echo ""
echo "  bundle: $DIST/mole"
# Apparent size, not disk usage: this machine's filesystem compresses, so `du -sh`
# reported 807K one run and 143M another for the same bundle. A figure nobody can
# reproduce is worse than none -- see MOLE-296.
echo "  size:   $(du -sh --apparent-size "$DIST" | cut -f1) (unpacked)"
