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
grep -q '; do$' <<<"$(printf '%s' "$groups" | tail -1)" \
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
grep -q 'exit 1' <<<"$(sed -n '/NEEDED.*libQt6Multimedia/,/^fi$/p' "$BUNDLE")" \
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
grep -q "Apache-2.0" <<<"$(sed -n '/is_excluded()/,/^}/p' "$BUNDLE")" \
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

begin "a GPL-3 library cannot travel inside the bundle, and the reason is written down"
# libsmbclient is GPL-3.0-or-later and Mole is Apache-2.0. It cannot be an
# exclusion the way the codec stack is -- it is linked into the network plugin, so
# leaving the .so out would make the plugin unloadable and take SFTP, FTP, S3 and
# WebDAV with it. The answer is upstream, at configure time, and this is the guard
# that says so. See ADR-0094.
grep -q 'libsmbclient' "$BUNDLE" \
    || fail "nothing stops a bundle carrying libsmbclient"
grep -q 'MOLE_WITH_SMB=OFF' "$BUNDLE" \
    || fail "the guard does not name the option that fixes it, so it is a dead end"
grep -q 'GPL-3' "$BUNDLE" \
    || fail "the licence reason for refusing libsmbclient is not stated where the refusal is"

begin "the self-contained artefacts are configured without SMB"
# Asserted of both, because the fault is one of them quietly finding the library.
grep -q 'MOLE_WITH_SMB=OFF' "$MOLE_SOURCE_DIR/Makefile" \
    || fail "make bundle builds with Windows shares, so the tarball would carry libsmbclient"
grep -q 'MOLE_WITH_SMB=OFF' "$MOLE_SOURCE_DIR/scripts/package-appimage.sh" \
    || fail "the AppImage builds with Windows shares, so it would carry libsmbclient"
# The assertion itself now lives in the one feature list all three consumers read
# -- see scripts/feature-summary.sh and MOLE-387 -- so both halves are checked:
# the list says it, and the packer reads the list.
grep -q 'Windows shares: not built' "$MOLE_SOURCE_DIR/scripts/feature-summary.sh" \
    || fail "the feature list does not assert the absence, so dropping the flag would go unnoticed"
grep -q 'feature-summary.sh' "$MOLE_SOURCE_DIR/scripts/package-appimage.sh" \
    || fail "the AppImage packer does not check itself against the feature list"

begin "a bundle on this machine carries no libsmbclient"
# The behavioural half, where there is something to look at.
if [ ! -d dist/usr ]; then
    echo "  skipped: no bundle in dist/ -- run make bundle"
else
    found=$(find dist -name 'libsmbclient*' 2>/dev/null | head -1)
    [ -z "$found" ] || fail "the bundle carries $found, which is GPL-3.0-or-later"
    while IFS= read -r object; do
        grep -q 'NEEDED.*libsmbclient' <<<"$(objdump -p "$object" 2>/dev/null)" \
            && fail "$object still asks for libsmbclient, so the bundle is short of it"
    done < <(find dist/usr -name '*.so' -o -type f -perm -u+x 2>/dev/null)
fi

begin "the bundler strips over the directories it discovered, not a spelled-out path"
# The structural half. `make bundle` and package-appimage.sh each carried a strip
# line naming lib/mole/plugins, and the AppImage is built on AlmaLinux 9 where
# GNUInstallDirs gives lib64 -- so those globs matched nothing, `|| true` hid the
# complaint, and both plugins shipped with their debug sections. They statically
# link their backend and mole_core, so that is MOLE-296 "51 MB of debug symbols in
# an artefact people download" in a second place. See MOLE-387.
grep -q 'MOLE_PLUGIN_DIRS' scripts/make-bundle.sh \
    || fail "the bundler has no discovered plugin directories"
stripping="$(awk '/^echo "  stripping"/,/^# The launcher/' scripts/make-bundle.sh)"
grep -q 'MOLE_PLUGIN_DIRS' <<<"$stripping" \
    || fail "the stripping step does not cover the plugin directories it found"
# And the two copies are gone.
grep -qE '^\t@strip .*lib/mole' Makefile \
    && fail "make bundle still strips a hard-coded lib/mole path"
grep -qE '^\s*strip .*lib/mole' scripts/package-appimage.sh \
    && fail "the AppImage packer still strips a hard-coded lib/mole path"

begin "the published plugins are stripped"
# The behavioural half, where there is a bundle to look at. A stripped object has
# no .debug_info section; an unstripped plugin here is a third of the tarball.
plugins=$(find dist/usr -type d -path '*/mole/plugins' 2>/dev/null | head -1)
if [ -z "$plugins" ]; then
    echo "  skipped: no bundle in dist/ -- run make bundle"
elif ! command -v readelf >/dev/null; then
    echo "  skipped: no readelf on this machine"
else
    while IFS= read -r object; do
        [ -n "$object" ] || continue
        sections="$(readelf -S "$object" 2>/dev/null)"
        grep -q 'debug_info' <<<"$sections" \
            && fail "$object still carries its debug sections"
    done < <(find "$plugins" -name '*.so' 2>/dev/null)
    # And the binaries beside them, which is what MOLE-296 was about.
    while IFS= read -r object; do
        [ -n "$object" ] || continue
        sections="$(readelf -S "$object" 2>/dev/null)"
        grep -q 'debug_info' <<<"$sections" \
            && fail "$object still carries its debug sections"
    done < <(find dist/usr/bin -type f 2>/dev/null)
fi

done_testing
