#!/usr/bin/env bash
#
# The .deb and the .rpm, held by reading what makes them.
#
# Building one takes a release build and installing one takes a container, so
# neither belongs in a suite anybody runs on every change: that is what
# `make packages` and the release workflow are for, and both were run by hand
# against clean containers of each family before this landed.
#
# **Two of the cases here are about compiling somewhere else rather than about a
# package.** They are here because `make rpm` is the only cross-distribution build
# this project has, and both faults it found stop the .rpm being built at all: a
# plugin class name Qt 6.10 refuses, and a libnfs whose read and write swapped
# their arguments in 6.0. Neither can fail on this machine, which is the whole
# reason they are read rather than compiled. See MOLE-389.
#
# What is worth holding here is the part that goes stale silently. CPack packs what
# `make install` installs, so the layout cannot drift -- but the dependencies a
# package declares can, in one specific place: Qt loads the QML modules and the SQL
# driver at run time, nothing links them, and so no tool derives them. They are the
# one typed list in the packaging, and a module the interface starts importing
# without the list being told is a package that installs and then will not start.
# See MOLE-121.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

begin "the packages are made from the install rules and nothing else"
grep -q 'include(CPack)' CMakeLists.txt || fail "nothing includes CPack"
# /usr, not the /usr/local a local install uses: a distribution package in
# /usr/local is in the place reserved for what the administrator put there.
grep -q 'set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")' CMakeLists.txt \
    || fail "the packages do not install into /usr"
# Derived dependencies. dpkg-shlibdeps reads the binaries and says what this build
# actually linked; a list typed once declares too little or too much.
grep -q 'set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)' CMakeLists.txt \
    || fail "the .deb does not derive its dependencies from what was linked"

begin "the .deb asks for the decoders Qt Multimedia is a front end to"
# The third thing nothing links and so nothing derives, beside the QML modules and
# the SQL driver. Measured on a clean ubuntu:24.04 with the .deb installed and
# nothing else: the platform default is GStreamer, whose decoders are in separate
# packages, so video preview reported no codecs at all. Recommends rather than
# Depends is deliberate -- see the note beside it -- so this asks for the
# relationship it should have rather than merely for a mention. See MOLE-317.
if grep -q 'MOLE_HAVE_MULTIMEDIA' CMakeLists.txt; then
    recommends=$(sed -n 's/^ *set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "\(.*\)").*/\1/p' CMakeLists.txt)
    [ -n "$recommends" ] || fail "the .deb recommends nothing, so a plain install decodes no video"
    for package in gstreamer1.0-plugins-good gstreamer1.0-libav; do
        printf '%s' "$recommends" | grep -qF "$package" \
            || fail "the .deb does not ask for $package"
    done
fi

begin "the packaged version is the one place the version lives"
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt)
[ -n "$version" ] || fail "cannot tell what version the repository is at"
: > "$SHELLTEST_TMP/spelled"
for file in CMakeLists.txt Makefile scripts/package-rpm.sh .github/workflows/release.yml; do
    # Every mention except the one that holds it: project(VERSION ...) is the place
    # everything else reads, which is the whole of MOLE-117.
    grep -nF "$version" "$file" 2>/dev/null | grep -vE '^[0-9]+: *VERSION ' \
        >> "$SHELLTEST_TMP/spelled"
done
if [ -s "$SHELLTEST_TMP/spelled" ]; then
    fail "the version is spelled out where the packaging can read it instead"
    sed 's/^/    /' "$SHELLTEST_TMP/spelled"
fi
grep -q 'set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")' CMakeLists.txt \
    || fail "the packages do not take their version from project(VERSION)"

begin "every QML module the interface imports is a package the .deb asks for"
# The one list nothing can derive, so this is the case that keeps it honest. An
# import is resolved to the package that owns the module directory, which is a
# question only a dpkg machine can answer -- said rather than skipped silently.
if ! command -v dpkg >/dev/null 2>&1; then
    echo "  skipped: no dpkg on this machine, so a module cannot be traced to a package"
else
    qmldir="/usr/lib/$(uname -m)-linux-gnu/qt6/qml"
    : > "$SHELLTEST_TMP/unasked"
    imports=$(grep -rhoE '^import Qt[A-Za-z0-9.]*' src/app/ui/*.qml | sed 's/^import //' | sort -u)
    count=$(printf '%s\n' "$imports" | grep -c .)
    [ "$count" -ge 4 ] || fail "only $count QML imports found; the parse has stopped working"
    for module in $imports; do
        path="$qmldir/$(printf '%s' "$module" | tr '.' '/')/qmldir"
        [ -e "$path" ] || continue # not a module with a directory of its own
        owner=$(dpkg -S "$path" 2>/dev/null | cut -d: -f1)
        [ -n "$owner" ] || continue # built from source rather than installed
        grep -qF "$owner" CMakeLists.txt || echo "$module comes from $owner, which the packaging never names" \
            >> "$SHELLTEST_TMP/unasked"
    done
    if [ -s "$SHELLTEST_TMP/unasked" ]; then
        fail "a package that installs and then will not start: nothing links a QML module"
        sed 's/^/    /' "$SHELLTEST_TMP/unasked"
    fi
    # The other half of the same class: the SQL driver, which every table, index and
    # scratch database goes through and which nothing links either.
    grep -q 'libqt6sql6-sqlite' CMakeLists.txt || fail "the .deb does not ask for the SQLite driver"
fi

begin "the bundle judges its libraries rather than resolving them"
# Two rules from MOLE-302, and both are about a check being able to fail.
#
# A dependency is judged against the two places it may come from -- inside the
# bundle, or named by is_excluded -- and not against whether it happens to resolve on
# the machine doing the build. `ldd` searches the bundle and then the host, so a
# library excluded by mistake that the build machine has resolves, reports found, and
# passes, while the artefact goes out without it. MOLE-300 was caught only because
# this workstation was also missing libxcb-cursor0.
# Held by what it asks rather than by what it does not: the objects' own NEEDED
# entries, and is_excluded for the ones the host is meant to supply. Reverting to
# resolution would take both of those away. (`ldd` is still right one step earlier,
# where the question really is whether a plugin can load on this machine.)
grep -q "objdump -p" scripts/make-bundle.sh \
    || fail "the completeness check does not read what the objects declare they need"
# `[$]` and not a backslash: MOLE-233's rule refuses deferred expansion in any
# script here, and this is a pattern to match with rather than a variable to expand.
grep -qE 'is_excluded "[$]name"' scripts/make-bundle.sh \
    || fail "the completeness check does not consult the exclusion list"

# And nothing is excluded by resemblance. A name in that list is a judgement that
# every target has that library; a pattern in the middle of a name is a judgement
# about everything that will ever look like it, which is not a judgement anybody
# made -- it is how libxcb-cursor came to be left out of an artefact that could not
# start without it.
: > "$SHELLTEST_TMP/resemblance"
# Comments are skipped: the list explains itself by quoting the pattern it used to
# end with, and a rule that read prose as code would fire on its own explanation.
awk '/^is_excluded\(\)/,/^}/' scripts/make-bundle.sh | grep -v '^[[:space:]]*#' \
    | grep -oE '\blib[A-Za-z0-9_+-]*\*[A-Za-z0-9_.+-]*' \
    | grep -vE '\.so\*$|^libwayland\*$|^ld-linux\*$' > "$SHELLTEST_TMP/resemblance"
if [ -s "$SHELLTEST_TMP/resemblance" ]; then
    fail "a library is excluded by resemblance rather than by name"
    sed 's/^/    /' "$SHELLTEST_TMP/resemblance"
fi

begin "the .rpm is built on the family it installs on"
# Not a preference: rpmbuild records what the binaries link, and a Debian libcurl
# carries symbol versions no RPM distribution provides, so an .rpm built here is
# refused by dnf outright. The Makefile must not reach for cpack -G RPM directly.
grep -q 'scripts/package-rpm.sh' Makefile || fail "make rpm does not go through the container script"
grep -nE '^\s*@?cd .*cpack -G RPM' Makefile > "$SHELLTEST_TMP/native-rpm"
if [ -s "$SHELLTEST_TMP/native-rpm" ]; then
    fail "the Makefile builds an .rpm on this machine, which cannot be installed on that one"
    sed 's/^/    /' "$SHELLTEST_TMP/native-rpm"
fi
grep -q 'CURL_OPENSSL_4' scripts/package-rpm.sh \
    || fail "the script does not say why it uses a container"

begin "the .rpm is installed somewhere other than the image that built it"
# The install check ran in fedora:40 and `make rpm` built in fedora:40, so what it
# asked was whether a package installs on the system that made it -- a fixture that
# cannot be false, TODO.md rule four. An .rpm records the sonames its binaries link
# and the build container decides them, so the only question worth the docker pull
# is whether a *different* system can satisfy them. See MOLE-389.
build_image=$(sed -n 's/^IMAGE="${MOLE_RPM_IMAGE:-\(.*\)}"$/\1/p' scripts/package-rpm.sh)
[ -n "$build_image" ] || fail "cannot tell which image scripts/package-rpm.sh builds in"

# The images actually handed to docker, rather than every one the file mentions --
# rule three, skip the file's own account of itself: the comment beside the check
# names the retired image it used to use, and that is prose about the fault.
grep -A2 -E '\bdocker run\b' .github/workflows/release.yml \
    | grep -oE '(registry\.fedoraproject\.org/)?fedora[a-z-]*:[a-z0-9.]+' \
    | sort -u > "$SHELLTEST_TMP/install-images"
[ -s "$SHELLTEST_TMP/install-images" ] \
    || fail "the release workflow installs the .rpm in no Fedora container at all"
while read -r image; do
    [ "$image" != "$build_image" ] \
        || fail "the .rpm is only installed in $image, which is what built it"
done < "$SHELLTEST_TMP/install-images"

begin "every plugin class name is a name C++ could compile"
# `CLASS_NAME mole::ArchivePlugin` sat in src/plugins/CMakeLists.txt for months and
# configured cleanly, because Qt 6.4 stores whatever it is handed and only a static
# plugin ever uses it. Qt 6.10 validates it, and refused to configure at all --
# found by moving the .rpm off a Fedora that had stopped moving, which is the whole
# argument for not pinning one. The value is only ever the argument of a
# `Q_IMPORT_PLUGIN(...)`, and what moc exports for a namespaced plugin class is the
# unqualified symbol, so a qualified name was never a name that could work. Here
# rather than in a compile: on Qt 6.4 there is nothing to compile, which is exactly
# how it went unnoticed. See MOLE-389.
grep -rh --include=CMakeLists.txt "CLASS_NAME" src CMakeLists.txt 2>/dev/null \
    | sed 's/^ *//' | sort -u > "$SHELLTEST_TMP/class-names"
[ -s "$SHELLTEST_TMP/class-names" ] || fail "no plugin declares a CLASS_NAME, so this case reads nothing"
while read -r line; do
    name=${line##*CLASS_NAME }
    name=${name%% *}
    printf '%s' "$name" | grep -qE '^[A-Za-z_][A-Za-z0-9_]*$' \
        || fail "CLASS_NAME $name is not a C++ identifier, and Qt 6.10 refuses to configure"
done < "$SHELLTEST_TMP/class-names"

begin "libnfs's read and write go through the pair that knows both orders"
# libnfs 6.0 put the buffer before the count, POSIX order, where 5.x took the count
# first. Both are `nfs_read`, both take a uint64 and a pointer, and there is no
# version macro to test -- so the only report is a compiler with the other header
# refusing the call, which happened the first time anything was built on a Fedora
# that had moved. `nfsRead` and `nfsWrite` in NfsFileSystem.cpp ask the declaration
# which order it has; a call written directly compiles here and nowhere newer.
# See MOLE-389.
grep -rn --include="*.cpp" --include="*.h" -E '\bnfs_(read|write)\(' src \
    | grep -vE 'nfs_(read|write)\(context, handle,' > "$SHELLTEST_TMP/direct-nfs"
if [ -s "$SHELLTEST_TMP/direct-nfs" ]; then
    fail "a libnfs read or write is called directly, and its argument order moved in 6.0"
    sed 's/^/    /' "$SHELLTEST_TMP/direct-nfs"
fi
# And the pair is still there to be called: a rule whose subject has been renamed
# passes by finding nothing.
grep -q 'int nfsRead(' src/plugins/network/NfsFileSystem.cpp \
    || fail "nothing in NfsFileSystem.cpp adapts libnfs's read"
grep -q 'int nfsWrite(' src/plugins/network/NfsFileSystem.cpp \
    || fail "nothing in NfsFileSystem.cpp adapts libnfs's write"

begin "nothing in the packaging is pinned to a Fedora release number"
# A release number is a pin with an expiry date, and this expired: the .rpm, the
# install check and the weekly job all named fedora:40, whose support ended more
# than a year before anybody noticed. Nothing was red -- an unsupported release
# stops moving, so the job that exists to notice movement kept answering the same
# question -- while the published .rpm would not install on any Fedora a user has.
# ADR-0081 rejected a pinned image for this reason and a release number is that pin
# by another route. See MOLE-389.
second_family=$(sed -n 's/^ *container: *\([^ ]*\).*$/\1/p' .github/workflows/second-family.yml)
[ -n "$second_family" ] || fail "cannot tell which image the weekly job runs in"
for image in "$build_image" "$second_family" $(cat "$SHELLTEST_TMP/install-images"); do
    case "${image##*:}" in
        *[0-9]*) fail "$image names a release, and a release goes out of support" ;;
    esac
done

begin "every optional library is in ARCHITECTURE.md's table"
# The section said "three features depend on libraries that may not be installed"
# and listed four, while the tree had eleven -- so the document was wrong about
# the number, wrong about the list, and had been for long enough that nobody
# noticed either. Held against the calls that do the finding, so a row added to
# the build has to be written down before the suite is green again. See MOLE-390.
rows=$(python3 "$MOLE_SOURCE_DIR/tests/support/read-optional-dependencies.py" rows)
count=$(printf '%s\n' "$rows" | grep -c .)
[ "$count" -ge 10 ] || fail "only $count optional-dependency rows were read; the parse has stopped working"
: > "$SHELLTEST_TMP/undocumented"
while IFS=$'\t' read -r name summary define target switch file; do
    [ -n "$summary" ] || continue
    # In the table, which is what a reader looks at -- a row mentioned only in the
    # prose above it is not the list.
    grep -qE "^\| *$summary *\|" ARCHITECTURE.md \
        || printf '%s (%s)\n' "$summary" "$name" >> "$SHELLTEST_TMP/undocumented"
done <<< "$rows"
if [ -s "$SHELLTEST_TMP/undocumented" ]; then
    fail "an optional library the build looks for is in no row of ARCHITECTURE.md's table"
    sed 's/^/    /' "$SHELLTEST_TMP/undocumented"
fi
# And the number in the prose, because that is the sentence that was wrong.
grep -qE "^\*\*$(printf '%s' "$count" | sed 's/^11$/Eleven/; s/^10$/Ten/; s/^12$/Twelve/') features depend on libraries" ARCHITECTURE.md \
    || fail "the section does not say there are $count of them"

begin "the digest of what the AppImage build downloads and runs is recorded"
# appimagetool is fetched over a URL and then executed, and a pinned version is
# not a pinned file: a release asset can be replaced under its tag. Apache's
# arrow apt source is fetched and given root, and cannot be pinned by digest --
# `latest` is its name -- so that one is checked by signature instead. See
# MOLE-390 and scripts/arrow-apt-source.sh.
grep -qE '^TOOL_SHA256="[0-9a-f]{64}"' scripts/package-appimage.sh \
    || fail "no sha256 is recorded for the tool that packs the AppImage"
grep -q 'sha256sum /tmp/appimagetool' scripts/package-appimage.sh \
    || fail "the recorded sha256 is never compared with the file"
[ -x scripts/arrow-apt-source.sh ] || fail "scripts/arrow-apt-source.sh is not there or not executable"
grep -q 'gpg --batch --status-fd 1 --verify' scripts/arrow-apt-source.sh \
    || fail "the arrow apt source is installed without its signature being checked"
grep -qE 'KEY_FINGERPRINT="[0-9A-F]{40}"' scripts/arrow-apt-source.sh \
    || fail "no key fingerprint is recorded for the signature to be compared against"
# And nothing installs it the old way, in either workflow.
grep -n 'apache-arrow-apt-source' .github/workflows/*.yml > "$SHELLTEST_TMP/raw-arrow"
if [ -s "$SHELLTEST_TMP/raw-arrow" ]; then
    fail "a workflow still fetches the arrow apt source itself, so the check is bypassed"
    sed 's/^/    /' "$SHELLTEST_TMP/raw-arrow"
fi

begin "packaging carries the AppStream metainfo a desktop application is expected to have"
# lintian warns on a .desktop file with no metainfo, AppImageHub asks for one, and
# a desktop that shows applications reads it for the name, the summary and the
# screenshots. There was none: the .desktop file and the icon were installed and
# nothing described the application. See MOLE-390.
# Named for its component id, which is what AppStream asks of the filename.
metainfo=$(find packaging -name '*.metainfo.xml' | head -1)
[ -n "$metainfo" ] || fail "there is no metainfo file in packaging/ at all"
for wanted in "<id>" "<name>" "<summary>" "<description>" "<project_license>" "<metadata_license>"; do
    grep -qF "$wanted" "$metainfo" || fail "$metainfo has no $wanted"
done
# The id has to match the desktop file, or a desktop shows two entries for one
# application -- one from each.
desktop=$(basename packaging/mole.desktop)
grep -qE "<launchable type=\"desktop-id\">$desktop</launchable>" "$metainfo" \
    || fail "the metainfo does not name $desktop, so a desktop sees two applications rather than one"
grep -qE "install\(FILES [^)]*$(basename "$metainfo")" CMakeLists.txt \
    || fail "the metainfo file is not installed, so no package carries it"
# And where a desktop looks for it.
grep -q 'DATAROOTDIR}/metainfo' <<< "$(grep -A2 -F "$(basename "$metainfo")" CMakeLists.txt)" \
    || fail "the metainfo file is installed somewhere other than share/metainfo"

begin "the AppImage's floor is written where a downloader can find it"
# What it was built on decides what it runs on, so the floor is a promise. A promise
# that lives only in a script is one that moves the day somebody bumps a base image,
# which is why this case reads the script's own base and holds the two places a
# reader would look to it.
floor=$(sed -n 's/^IMAGE="${MOLE_APPIMAGE_BASE:-\([^}]*\)}"$/\1/p' scripts/package-appimage.sh)
[ -n "$floor" ] || fail "cannot tell what the AppImage is built on"
# The glibc version that image carries, said in the script and repeated wherever the
# promise is made. Read from the script rather than written here, for the same
# reason.
# The sentence that states it, not any mention of a glibc: the script explains the
# fault by naming the version a *newer* build would demand, and picking that one up
# would have this case checking the wrong number -- which it did, first time.
version=$(sed -n 's/.*floor is glibc \(2\.[0-9]*\).*/\1/p' scripts/package-appimage.sh | head -1)
[ -n "$version" ] || fail "the script does not say what its floor is"
#
# One phrase, and it has to be that phrase: "runs on glibc <floor>". Anything looser
# passes on prose that merely mentions a version -- both of these first passed while
# TODO.md said the wrong number, because the note explains the floor by naming other
# distributions' versions too. If the wording here wants changing, change it in all
# three places; that is the point of the case.
# TODO.md is where the reasoning is kept and README.md is what somebody choosing an
# artefact reads. The release body used to say it too and no longer can: MOLE-123
# made that body exactly the changelog block, which is a list of changes rather than
# a description of what is attached.
for place in TODO.md README.md; do
    grep -qF "runs on glibc $version" "$place" \
        || fail "$place does not promise 'runs on glibc $version'"
done
grep -qiE "built on \*{0,2}${floor%%:*}" TODO.md \
    || fail "TODO.md does not say it is built on ${floor%%:*}"
# Pinned, because the tool that packs a release is part of the release. Asked of the
# url alone: the script says the word "continuous" while explaining why it is not
# using one.
tool=$(sed -n 's/^TOOL_URL="\(.*\)"$/\1/p' scripts/package-appimage.sh)
printf '%s\n' "$tool" | grep -q 'releases/download/[0-9]' \
    || fail "appimagetool is not pinned to a release: $tool"
printf '%s\n' "$tool" | grep -q 'continuous' \
    && fail "appimagetool is taken from a continuous build"

begin "the AppImage carries what the format needs"
# An AppRun, and the desktop entry and icon at the top level -- a desktop that
# integrates AppImages reads them from there, not from usr/share.
for piece in 'AppRun' 'mole.desktop' 'mole.svg'; do
    grep -qF "$piece" scripts/package-appimage.sh || fail "the AppDir has no $piece"
done
# And the licence check stays a hard failure, run against the artefact rather than
# against this repository.
grep -q 'licence-check.sh' scripts/package-appimage.sh || fail "the AppImage skips the licence check"

begin "a packaging script that fails fails the build"
# `make rpm` and `make appimage` ended in `|| true`, written for "the tool is not
# on this machine" -- and both scripts already exit 3 with a printed `skipped:`
# for exactly that, and non-zero for every other failure. The Makefile swallowed
# them alike, so release.yml's "The .deb, the .rpm and the AppImage" step was
# green whatever the three builds did, and the release stopped two steps later
# when `dnf install` could not find a file, naming the wrong fault. TODO.md rule
# one: a check has to be able to fail. See MOLE-387.
#
# Driven with stubs rather than by building anything: what is under test is the
# Makefile, and the two real scripts need docker and twenty minutes.
stubs="$(mktemp -d)"
trap 'rm -rf "$stubs"' EXIT
mkdir -p "$stubs/scripts"
for one in package-rpm package-appimage; do
    printf '#!/bin/sh\necho "boom" >&2\nexit 1\n' > "$stubs/scripts/$one.sh"
    chmod +x "$stubs/scripts/$one.sh"
done
cp Makefile "$stubs/Makefile"
if make -C "$stubs" rpm >/dev/null 2>&1; then
    fail "make rpm passed with a packaging script that exited 1"
fi
if make -C "$stubs" appimage >/dev/null 2>&1; then
    fail "make appimage passed with a packaging script that exited 1"
fi
# And exit 3 -- the tool is not here -- is still a skip rather than a failure,
# which is the whole reason the `|| true` was there.
for one in package-rpm package-appimage; do
    printf '#!/bin/sh\necho "skipped: no tool here" >&2\nexit 3\n' > "$stubs/scripts/$one.sh"
done
make -C "$stubs" rpm >/dev/null 2>&1 || fail "make rpm failed on a skip"
make -C "$stubs" appimage >/dev/null 2>&1 || fail "make appimage failed on a skip"

begin "every container script can fail"
# A pipeline takes the status of its last command, so `set -e` alone does not stop
# a script whose checks end in tee, tail or grep. Both packaging scripts run a
# body through `bash -c`, and both bodies said `set -e`. See MOLE-387.
for script in scripts/package-rpm.sh scripts/package-appimage.sh; do
    grep -q 'set -eo pipefail' "$script" \
        || fail "the container body in $script does not set pipefail"
done
# And the packer's own status is not thrown away by a pipe into tail.
grep -q 'appimagetool "$APPDIR" "$image"' scripts/package-appimage.sh \
    || fail "appimagetool is not called with its status kept"
grep -qF '[ -s "$image" ]' scripts/package-appimage.sh \
    || fail "nothing checks that an image was actually produced"

begin "the bundler strips the plugin directories it discovered"
# package-appimage.sh and `make bundle` each had a strip line naming
# usr/lib/mole/plugins -- and the AppImage is built on AlmaLinux 9, where
# GNUInstallDirs gives lib64. The glob matched nothing and `|| true` hid it, so
# both plugins shipped with their debug sections: they statically link their
# backend and mole_core, which is MOLE-296 all over again. See MOLE-387.
grep -q 'for dir in "$LIBDIR" "$PLUGINDIR" "$QMLDIR" $MOLE_PLUGIN_DIRS' scripts/make-bundle.sh \
    || fail "the bundler does not strip over the plugin directories it found"
grep -qE '^\s*strip .*lib/mole' scripts/package-appimage.sh \
    && fail "the AppImage packer still strips a hard-coded lib/mole path"
grep -qE '^\t@strip .*usr/lib/mole' Makefile \
    && fail "make bundle still has a hard-coded lib/mole/plugins strip"

begin "the feature summary is written once"
# Three consumers were carrying their own copy -- release.yml, the AppImage packer
# and tst_ReleaseWorkflow.sh -- and the two that mattered had already drifted: the
# AppImage's was missing xxhash and the Multimedia QML module. See MOLE-387.
[ -f scripts/feature-summary.sh ] || fail "there is no shared feature summary"
grep -q 'feature-summary.sh' scripts/package-appimage.sh \
    || fail "the AppImage packer does not read the shared feature summary"
grep -q 'feature-summary.sh' .github/workflows/release.yml \
    || fail "the release workflow does not read the shared feature summary"
# And the artefact-specific exemptions are named in it rather than being the
# difference between two lists.
grep -q 'Windows shares: not built' scripts/feature-summary.sh \
    || fail "the AppImage exemption is not named in the shared list"

begin "all three packaging targets exist and are declared"
for target in packages deb rpm appimage; do
    grep -qE "^$target:" Makefile || fail "there is no target called $target"
    grep -qE "^\.PHONY:.*\b$target\b" Makefile || fail "$target is not in .PHONY"
done

done_testing
