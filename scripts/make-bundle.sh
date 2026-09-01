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

        # **The media codec stack, left to the host for a licence reason rather than
        # a coupling one -- which makes this a different kind of entry from every
        # group above.**
        #
        # A distribution's ffmpeg is built against whatever that distribution is
        # willing to ship, and what it links is not a list this project chose. On
        # Ubuntu that closure includes libx264 and libx265 (GPL-2+) and libxvidcore
        # and libzvbi (GPL-2+); the AppImage's AlmaLinux closure adds libfdk-aac,
        # which is not free software at all. Mole is Apache-2.0. A self-contained
        # artefact carrying any of them is a combined work nobody may redistribute,
        # and v0.1.0 went out carrying several. See MOLE-322.
        #
        # **Excluding them one name at a time is what produced this**, so the rule is
        # the stack rather than the offenders: none of libav*, libsw* or libpostproc
        # is bundled, and with nothing left needing them their whole codec closure
        # stops being collected too. What the artefact carries is then decided by
        # this project rather than by a distribution's packaging policy.
        #
        # Qt's own `libffmpegmediaplugin.so` is still bundled -- it is LGPL like the
        # rest of Qt. It loads where the host has ffmpeg and does not where it has
        # not, which is the arrangement the author asked for: a machine with the
        # codecs decodes video, a machine without gets the file-information view for
        # one and is told so by `mole --plugins`. MOLE-316 is what makes the second
        # case survivable rather than an abort, and README.md says what to install.
        libavcodec.so* | libavformat.so* | libavutil.so* | libavfilter.so* \
            | libavdevice.so* | libswscale.so* | libswresample.so* | libpostproc.so*)
            return 0 ;;

        *) return 1 ;;
    esac
}

# Walks dependencies to a fixed point: a copied Qt library pulls in more Qt
# libraries, so one pass is never enough.
#
# **Each object's own NEEDED entries, not `ldd`'s flattened closure**, and the
# difference is the whole reason an exclusion works. `ldd` reports everything an
# object reaches, however indirectly, so excluding a library did nothing about what
# *it* needs: libavcodec was refused and libx264, libx265 and libxvidcore -- its
# dependencies, GPL-2+ every one -- were copied anyway, because they were still in
# the plugin's `ldd` output. An exclusion has to take the subtree with it or it is
# not an exclusion. See MOLE-322.
#
# `ldd` is still what resolves a soname to a path, because that is the question it
# answers well. The direct entries decide *whether*, the closure decides *where*.
# This is also the question the completeness check at the bottom of this file has
# always asked, so the collector and the check now agree rather than differing in a
# way nobody noticed.
collect_deps() {
    local changed=1
    while (( changed )); do
        changed=0
        while IFS= read -r -d '' object; do
            local -A path_of=()
            local name path
            while read -r name path; do
                [[ -n "$path" && -f "$path" ]] && path_of["$name"]="$path"
            done < <(ldd "$object" 2>/dev/null | awk '/=> \//{print $1, $3}')

            while read -r name; do
                path="${path_of[$name]:-}"
                [[ -n "$path" ]] || continue
                is_excluded "$name" && continue
                [[ -f "$LIBDIR/$name" ]] && continue
                cp -L "$path" "$LIBDIR/$name"
                changed=1
            done < <(objdump -p "$object" 2>/dev/null | awk '/NEEDED/{print $2}')
        done < <(find "$DIST/usr/bin" "$LIBDIR" "$PLUGINDIR" "$QMLDIR" $MOLE_PLUGIN_DIRS -type f \
                      \( -name '*.so*' -o -perm -u+x \) -print0 2>/dev/null)
    done
}

echo "  collecting Qt plugins"
for group in platforms platformthemes sqldrivers imageformats iconengines multimedia \
             xcbglintegrations wayland-shell-integration wayland-decoration-client \
             wayland-graphics-integration-client; do
    [[ -d "$QT_ROOT/plugins/$group" ]] || continue
    mkdir -p "$PLUGINDIR/$group"
    cp -a "$QT_ROOT/plugins/$group/." "$PLUGINDIR/$group/"
done

# **Of the two media backends only one can be bundled, and it is not the default.**
# Qt Multimedia is a front end; the plugin above is what decodes. Both were absent
# until now, which is why the AppImage reported no codecs on any machine however well
# equipped, and why `mole --plugins` from the tarball aborted outright on a machine
# with no Qt installed -- QMediaFormat::supportedVideoCodecs() asserts when there is
# no backend at all. See MOLE-317.
#
# The gstreamer backend cannot be made self-contained by anything this script does.
# Its NEEDED libraries are the GStreamer framework and nothing else; the 127 elements
# that do the actual work are dlopened at run time from the host's
# gstreamer-1.0 directory, so the dependency walk below cannot see them and would not
# copy them. Bundling it produces a backend with nothing behind it -- which is not a
# harmless absence but the exact state that segfaulted the release runner in
# MOLE-314: Qt reports no codecs, Mole tries anyway, and GStreamer dies linking a
# NULL element.
#
# The ffmpeg backend asks for libavcodec, libavformat, libswscale, libswresample,
# libavutil and libva, every one of them a NEEDED that the walk below collects. So it
# is the one that can travel, and it is named in the launcher rather than left to
# whichever Qt enumerates first.
if [[ -d "$PLUGINDIR/multimedia" ]]; then
    rm -f "$PLUGINDIR/multimedia/libgstreamermediaplugin.so"
    if [[ ! -e "$PLUGINDIR/multimedia/libffmpegmediaplugin.so" ]]; then
        echo "  this Qt has no ffmpeg media backend, so the bundle would carry none"
    fi
fi

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
# The one backend a bundle can carry -- see the note beside the plugin groups. Named
# rather than left to Qt's enumeration, so what this artefact decodes with is a fact
# and shows up in `mole --plugins`.
export QT_MEDIA_BACKEND=ffmpeg
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

# And the second one that is not optional, for a build that has Qt Multimedia in it.
# Asked of the binary rather than of a build flag, so it follows the build: if this
# links Qt6Multimedia then something will ask that library what it can decode, and
# with no backend the answer is an assertion rather than an empty list. A bundle
# without a backend is an application that ends the process on `mole --plugins`.
# See MOLE-317, and MOLE-316 for why the absence is fatal rather than disappointing.
if grep -q 'NEEDED.*libQt6Multimedia' <<<"$(objdump -p "$BIN" 2>/dev/null)"; then
    if ! compgen -G "$PLUGINDIR/multimedia/*.so" > /dev/null; then
        echo "  this build links Qt6Multimedia and the bundle carries no media backend,"
        echo "  so mole --plugins would abort on any machine without Qt installed."
        echo "  Install Qt's ffmpeg media plugin on this machine and run again --"
        echo "  libqt6multimedia6 carries it on Debian/Ubuntu."
        exit 1
    fi
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
