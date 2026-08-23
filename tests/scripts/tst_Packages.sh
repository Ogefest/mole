#!/usr/bin/env bash
#
# The .deb and the .rpm, held by reading what makes them.
#
# Building one takes a release build and installing one takes a container, so
# neither belongs in a suite anybody runs on every change: that is what
# `make packages` and the release workflow are for, and both were run by hand
# against clean containers of each family before this landed.
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
for place in TODO.md .github/workflows/release.yml; do
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

begin "all three packaging targets exist and are declared"
for target in packages deb rpm appimage; do
    grep -qE "^$target:" Makefile || fail "there is no target called $target"
    grep -qE "^\.PHONY:.*\b$target\b" Makefile || fail "$target is not in .PHONY"
done

done_testing
