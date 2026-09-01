#!/usr/bin/env bash
#
# The self-contained bundle, held by reading what makes it.
#
# Building one takes a release build and a few hundred megabytes, so it does not
# belong in a suite anybody runs on every change -- that is what `make bundle` and
# the release workflow are for. What is worth holding here is the part that went
# wrong and would go wrong again in silence: **which Qt plugin trees the bundle
# carries.**
#
# Qt loads plugins by looking in directories, and `make-bundle.sh` copies a list of
# them by name. A tree missing from that list is a feature the artefact silently has
# not got, and nothing else notices: `--plugins` counts Mole's plugins, not Qt's, and
# a bundle short of a Qt tree still starts, still shows a window, and still passes
# every other check. The multimedia tree was missing from the day the list was
# written, so every AppImage reported no codecs on every machine however well
# equipped, and `mole --plugins` from the tarball aborted outright on a machine with
# no Qt -- QMediaFormat::supportedVideoCodecs() asserts when there is no backend at
# all rather than returning an empty list. See MOLE-317, and MOLE-316 for why the
# absence is fatal rather than merely disappointing.
#
# The last case is the behavioural one and runs only where a bundle has been built.
# It says so when it skips, because a case that quietly asserts nothing is worse
# than no case.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

BUNDLE=scripts/make-bundle.sh

begin "there is a bundler and it parses as shell"
[ -f "$BUNDLE" ] || fail "$BUNDLE is not there at all"
bash -n "$BUNDLE" 2>"$SHELLTEST_TMP/parse" || {
    fail "$BUNDLE does not parse"
    sed 's/^/    /' "$SHELLTEST_TMP/parse"
}

begin "the bundle carries a media backend"
# The tree, in the list of plugin groups that get copied. Named rather than derived:
# what Qt calls its directories is Qt's vocabulary, not something this repository
# can compute.
# The range ends at `; do`, which is where the list ends. `/^do$/` was the first
# attempt and there is no such line -- the range ran to the end of the file, so the
# loop below found every group name somewhere in the rest of the script and the case
# passed while asserting nothing. A check that cannot fail is the fault this file
# exists to catch, so it is guarded twice: the list must end where it should, and it
# must be short enough to be a list.
groups=$(sed -n '/^for group in/,/; do$/p' "$BUNDLE")
[ -n "$groups" ] || fail "cannot find the list of Qt plugin groups the bundler copies"
printf '%s' "$groups" | tail -1 | grep -q '; do$' \
    || fail "the plugin-group list does not end where this expects, so the case below reads too much"
lines=$(printf '%s\n' "$groups" | grep -c .)
[ "$lines" -le 6 ] \
    || fail "the plugin-group list came back $lines lines long; the parse has stopped working"
for group in platforms sqldrivers imageformats multimedia; do
    printf '%s' "$groups" | grep -qw "$group" \
        || fail "the bundler does not collect Qt's $group plugins"
done

begin "the gstreamer backend is left out, because it cannot travel"
# Of the two backends only one can be bundled and it is not the platform default.
# The gstreamer plugin's NEEDED libraries are the framework alone; the elements that
# decode are dlopened at run time from the host's gstreamer directory, so the
# bundler's dependency walk cannot see them and would not copy them. Bundling it
# produces a backend with nothing behind it, which is not a harmless absence but the
# state that segfaulted the release runner in MOLE-314.
grep -q 'rm -f "$PLUGINDIR/multimedia/libgstreamermediaplugin.so"' "$BUNDLE" \
    || fail "the bundler does not drop the gstreamer backend, so the bundle would carry an empty one"

begin "the launcher names the backend rather than leaving it to be chosen"
# Qt picks a backend at run time and the platform default is the one that cannot
# work here. Named in the launcher, so what the artefact decodes with is a fact and
# shows up in `mole --plugins`.
launcher=$(sed -n '/^cat > /,/^LAUNCHER$/p' "$BUNDLE")
[ -n "$launcher" ] || fail "cannot find the launcher the bundler writes"
printf '%s' "$launcher" | grep -q 'QT_MEDIA_BACKEND=ffmpeg' \
    || fail "the launcher does not name a media backend, so Qt picks the one that cannot work"
# And the three the bundle has always needed, so a deletion here is caught too.
for variable in LD_LIBRARY_PATH QT_PLUGIN_PATH QML2_IMPORT_PATH; do
    printf '%s' "$launcher" | grep -q "$variable" \
        || fail "the launcher no longer sets $variable, so the bundle is not relocatable"
done

begin "a build with Qt Multimedia refuses to bundle without a backend"
# Beside the platform-plugin check and for the same reason: without it the artefact
# does not degrade, it ends the process. Asked of the binary rather than of a build
# flag, so it follows the build.
grep -q 'NEEDED.*libQt6Multimedia' "$BUNDLE" \
    || fail "nothing checks whether a build that links Qt Multimedia got a backend"
sed -n '/NEEDED.*libQt6Multimedia/,/^fi$/p' "$BUNDLE" | grep -q 'exit 1' \
    || fail "the media-backend check does not stop the bundle"

begin "an exclusion takes the subtree with it"
# The fault that put GPL-2+ libraries into a published release. The collector used
# `ldd`, which reports everything an object reaches however indirectly, so refusing
# libavcodec did nothing about what libavcodec needs: libx264, libx265, libxvidcore
# and libzvbi were still in the plugin's ldd output and were copied. An exclusion
# that does not take the subtree is not an exclusion. See MOLE-322.
#
# Asked of the collector rather than of a bundle, because a bundle only shows what
# this machine happened to have.
collector=$(sed -n '/^collect_deps()/,/^}/p' "$BUNDLE")
[ -n "$collector" ] || fail "cannot find the dependency collector"
printf '%s' "$collector" | grep -q 'objdump -p' \
    || fail "the collector does not read each object's own NEEDED entries, so an exclusion cannot prune"
printf '%s' "$collector" | grep -qE 'ldd .*\| *awk.*NEEDED' \
    && fail "the collector decides what to copy from ldd's flattened closure again"

begin "the media codec stack is left to the host, and the reason is written down"
# Not a preference about size. A distribution's ffmpeg links whatever that
# distribution ships, which on Ubuntu is four GPL-2+ libraries and on the AppImage's
# base adds a non-free one. Mole is Apache-2.0 and a bundle is one artefact.
for lib in libavcodec libavformat libavutil libswscale libswresample; do
    grep -qE "^ *\| *$lib\.so\*|$lib\.so\* *\|" "$BUNDLE" \
        || grep -q "$lib.so\*" "$BUNDLE" \
        || fail "$lib is not left to the host, so the bundle carries a distribution's codec choices"
done
sed -n '/is_excluded()/,/^}/p' "$BUNDLE" | grep -q "Apache-2.0" \
    || fail "the licence reason for excluding the codec stack is not stated where the exclusion is"

begin "the platform plugin is still the one that stops everything"
# The check this one was written beside. Kept here so that removing either is a
# failure rather than a quiet loss.
grep -q 'platforms/libqxcb.so' "$BUNDLE" \
    || fail "nothing checks that the bundle can start at all"

begin "a bundle on this machine carries no library it may not redistribute"
# The behavioural half. Named rather than derived, because a licence is not a fact
# any tool here can read off a .so -- these are the ones a distribution's ffmpeg
# pulls in that an Apache-2.0 artefact cannot absorb. See MOLE-322.
if [ ! -d dist/usr/lib ]; then
    echo "  skipped: no bundle in dist/ -- run make bundle"
else
    : > "$SHELLTEST_TMP/unredistributable"
    for pattern in libx264 libx265 libxvidcore libzvbi libfdk-aac; do
        found=$(find dist -name "$pattern*" 2>/dev/null | head -1)
        [ -z "$found" ] || echo "$found" >> "$SHELLTEST_TMP/unredistributable"
    done
    if [ -s "$SHELLTEST_TMP/unredistributable" ]; then
        fail "the bundle carries GPL or non-free libraries inside an Apache-2.0 artefact"
        sed 's/^/    /' "$SHELLTEST_TMP/unredistributable"
    fi
fi

begin "a bundle on this machine carries exactly one media backend"
# The behavioural half, where there is something to look at.
if [ ! -d dist/usr/plugins ]; then
    echo "  skipped: no bundle in dist/ -- run make bundle"
elif ! grep -q 'NEEDED.*libQt6Multimedia' <(objdump -p dist/usr/bin/mole 2>/dev/null); then
    echo "  skipped: this build does not link Qt Multimedia, so it needs no backend"
else
    backends=$(find dist/usr/plugins/multimedia -name '*.so' 2>/dev/null | sort)
    [ -n "$backends" ] || fail "the bundle in dist/ carries no media backend at all"
    printf '%s\n' "$backends" | grep -q gstreamermediaplugin \
        && fail "the bundle carries the gstreamer backend, whose decoders are not in it"
    # And it must not be short of anything, judged with the bundle's own libraries
    # ahead of the host's -- the plugin being present is not the same as loadable.
    for backend in $backends; do
        short=$(LD_LIBRARY_PATH=dist/usr/lib ldd "$backend" 2>/dev/null \
                | awk '/not found/{print $1}' | tr '\n' ' ')
        # What is_excluded leaves to the host is not a fault: those are the display
        # server's client side, which every desktop has and no container need.
        for name in $short; do
            case "$name" in
                libX*|libxcb-*|libGL*|libEGL*|libOpenGL*|libwayland*|libgcc_s*|libstdc++*) ;;
                *) fail "$backend is short of $name, so it would not load" ;;
            esac
        done
    done
fi

done_testing
