cmake_minimum_required(VERSION 3.24)

project(Mole
    VERSION 0.1.2
    DESCRIPTION "Extensible cross-platform file manager"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything is position-independent, because the plugins are shared objects that
# link the static libraries above them -- mole_sdk, and the archive and network
# backends. Without this the link fails outright on a distribution whose compiler
# does not happen to produce relocatable code by default:
#
#   relocation R_X86_64_32S against `.rodata' can not be used when making a
#   shared object; recompile with -fPIC
#
# Ubuntu's gcc got away with it and Fedora's does not, which is how it was found --
# building the .rpm on the family it installs on (MOLE-121). Set for every target
# rather than for the two that failed: the next static library linked into a plugin
# would fail the same way, and there is no reason for it to be a discovery.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Found, never demanded. A preset file cannot ask a question -- every entry in
# cacheVariables is an assertion -- so ccache in the base preset made a tool that
# happened to be installed on one machine a requirement of the build, and nothing
# configured at all on a machine without it. MSVC has never had it.
#
# This is the rule src/core/CMakeLists.txt states for every optional library, and
# the build had one place that broke it.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Compiler cache: ${CCACHE_PROGRAM}")
endif()

# One shape for every optional library. See the note at the top of the file --
# there were six shapes and the message they print is an interface.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MoleOptionalDependency)

option(MOLE_BUILD_TESTS "Build unit tests" ON)
option(MOLE_ENABLE_SANITIZERS "Enable ASan + UBSan" OFF)
# Separate from the one above, because the two cannot be built together: address
# and thread sanitizers instrument the same things and refuse to share a binary.
option(MOLE_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

# Core, its layers and their headless tests -- no QML, no plugins, no window.
#
# It exists for ThreadSanitizer. TSan can only be believed against a Qt whose own
# locking is annotated for it, which means a Qt built with `-sanitize thread`,
# and building all of Qt that way is not a thing to ask of anybody: QtPdf pulls
# in PDFium and QtMultimedia pulls in a media stack, for suites that have nothing
# to do with concurrency. qtbase alone is a tractable build and holds everything
# the concurrency suites need. See ADR-0055.
option(MOLE_CORE_ONLY "Build core, host, ui and their headless tests only" OFF)

# Windows shares, through libsmbclient.
#
# **On by default and off for the self-contained artefacts.** libsmbclient is
# GPL-3.0-or-later and Mole is Apache-2.0. Where the library comes from the
# distribution -- a .deb, an .rpm, a Flatpak, a local build -- there is nothing to
# reconcile and SMB is built. The AppImage and `make bundle` copy their libraries
# in beside the application, and that combined artefact is what somebody is handed;
# so those two are built without it, the same call MOLE-322 made about a
# distribution's video codecs.
#
# A build switch rather than an exclusion in the bundler, because libsmbclient is
# linked into the network plugin: leaving the library out of the bundle would make
# the whole plugin unloadable and take SFTP, FTP, S3 and WebDAV with it. See
# ADR-0094.
option(MOLE_WITH_SMB "Build Windows-share support (libsmbclient, GPL-3.0-or-later)" ON)

# Qt 6.4 is the baseline (Ubuntu 24.04 ships 6.4.2). Do not use newer APIs
# without bumping this number first -- it is what keeps the port to
# Windows/macOS honest.
if(MOLE_CORE_ONLY)
    # Everything here is in qtbase, which is the whole point -- see the option above.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Sql Concurrent)
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Sql Concurrent)
endif()

qt_standard_project_setup()

# Compile flags shared by every target in the project.
add_library(mole_flags INTERFACE)

# The version, and this is the only place in the repository that holds it.
#
# project(VERSION) above is where every packaging tool looks for it, so that is
# what owns it, and it reaches the code from here rather than being typed a second
# time -- `mole --version` is how somebody with a bug report says which build they
# are holding, and two copies agreeing today is not the same as staying true. The
# Makefile reads the same line out of this file. See MOLE-117.
target_compile_definitions(mole_flags INTERFACE MOLE_VERSION="${PROJECT_VERSION}")

target_compile_options(mole_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wnon-virtual-dtor>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)

if(MOLE_ENABLE_SANITIZERS AND MOLE_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "MOLE_ENABLE_SANITIZERS and MOLE_ENABLE_THREAD_SANITIZER cannot both be on")
endif()

if(MOLE_ENABLE_SANITIZERS)
    target_compile_options(mole_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mole_flags INTERFACE -fsanitize=address,undefined)
endif()

# Races are what the concurrency tests exist to find, and they are invisible to
# every other tool here: a task that reads a field another thread is writing
# passes every run until the day it does not. ThreadSanitizer is the only thing
# that sees one without it having to happen.
if(MOLE_ENABLE_THREAD_SANITIZER)
    target_compile_options(mole_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(mole_flags INTERFACE -fsanitize=thread)
endif()

# Strictly layered, each depending only on the ones above it:
#   core -> sdk -> host -> ui -> builtin -> app
add_subdirectory(src/core)
add_subdirectory(src/sdk)
add_subdirectory(src/host)
add_subdirectory(src/ui)
# The two that need Qml, Quick, Pdf or Multimedia, and so the two a qtbase-only
# Qt cannot build. Everything above needs nothing outside qtbase.
if(NOT MOLE_CORE_ONLY)
    add_subdirectory(src/plugins)
    add_subdirectory(src/app)
endif()

# **Below the plugins, and that is not a matter of taste.** The console runner
# links the archive writer and depends on the plugin targets, and it decides both
# on MOLE_HAVE_ARCHIVE_PLUGIN and MOLE_HAVE_NETWORK_PLUGIN -- which src/plugins
# writes into the cache, because find_package results do not cross between
# sibling directories.
#
# Added above them, as it was, those variables are undefined on a *fresh*
# configure and defined on every one after it: so a developer's frequently
# reconfigured tree could pack an archive and every published artefact -- every
# CI job, every packaging container, `make deb` -- shipped a mole-tasks whose
# `compress` said "this build has no archive support (libarchive was not
# found)". Untrue, and configure-count dependent, which is the worst kind of
# untrue. The suite was green either way because tests/ is configured after
# src/plugins. See MOLE-386, and tests/scripts/tst_TestSuiteRules.sh, which holds
# this order rather than trusting it.
add_subdirectory(src/tools)

if(MOLE_BUILD_TESTS)
    # Test alone here now. Network used to be found only for the suites -- Mole
    # talked to servers through libcurl and linked no HTTP client of its own -- and
    # it is above with the rest since the update check, which is Qt Network on the
    # event loop. What the suites still want it for is the other half: a server
    # that answers wrongly on purpose, which is the only way most of those answers
    # can be met at all. See tests/support/ScriptedHttpServer.h.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- installing
#
# Layout:
#   <prefix>/bin/mole
#   <prefix>/<libdir>/mole/plugins/*.so
#   <prefix>/share/applications, <prefix>/share/icons/..., <prefix>/share/metainfo
#
# `<libdir>` and not `lib`, which is what this said: GNUInstallDirs answers lib64
# on an RPM distribution, and the plugins went there while nothing looked for them
# -- no archive browsing and no network drives, reported as nothing at all rather
# than as a failure (MOLE-296). A path written as `lib` in a comment is how that
# came to be believed.
#
# PluginManager resolves the plugin directory relative to the executable, so
# the same build runs from any prefix without being rebuilt.
include(GNUInstallDirs)

set(MOLE_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/mole/plugins")

if(NOT MOLE_CORE_ONLY)
install(TARGETS mole mole-tasks
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Every plugin that was built, because a plugin left out here is a feature the
# artefact silently has not got. The network one was: it has been built since it
# replaced rclone and never installed, so `make install` and `make bundle` -- and
# so every release a workflow would publish -- had no sftp, ftp, s3 or webdav
# drives in them, while the machine it was built on had all four. Found by running
# the artefact rather than by reading these lines. See MOLE-120.
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    install(TARGETS mole_plugin_archive
        LIBRARY DESTINATION ${MOLE_PLUGIN_INSTALL_DIR})
endif()

if(MOLE_HAVE_NETWORK_PLUGIN)
    install(TARGETS mole_plugin_network
        LIBRARY DESTINATION ${MOLE_PLUGIN_INSTALL_DIR})
endif()
endif() # MOLE_CORE_ONLY: there is no application to install

# Licence paperwork travels with every install: Qt's LGPL requires the notice
# and its text to reach whoever receives the binary.
install(FILES LICENSE NOTICE THIRD-PARTY-NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/mole)
install(DIRECTORY licenses
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/mole)

# What a desktop needs to show the application, and only where there is one: all
# three of these sat outside the guard above, so a core-only install -- which
# builds no application at all, see MOLE_CORE_ONLY -- installed a desktop entry
# and an icon for a binary it had not built, and a menu offering it. See MOLE-390.
if(NOT MOLE_CORE_ONLY)
    install(FILES packaging/mole.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/mole.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
    # AppStream metainfo: what a desktop, an app store and lintian read about the
    # application. There was none, so every .deb carried a desktop entry nothing
    # described.
    install(FILES packaging/io.github.ogefest.mole.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
endif()

# ------------------------------------------------------------------ packaging
#
# A .deb and an .rpm are the install rules above and nothing else. CPack packs
# what `make install` installs, so the package and the install cannot come apart
# -- describing the layout a second time is exactly how they would. The desktop
# entry and the icon are already where freedesktop says, which is what both
# families read, so the application appears in a menu rather than only on a path.
# See MOLE-121.
if(NOT MOLE_CORE_ONLY)

set(CPACK_PACKAGE_NAME mole)
# From the one place the number lives -- see MOLE-117. `make release` rewrites
# that line and the packages follow.
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
# A file rather than a string, and it is not a nicety: the RPM generator asks
# CMake for a description and falls back to a template that Fedora's cmake package
# does not ship, so without this an .rpm cannot be generated on the family it is
# for. Found by building one in a Fedora container.
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/description.txt")
set(CPACK_PACKAGE_VENDOR "The Mole Authors")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Ogefest/mole")
# dpkg requires a maintainer and rpm a packager. The identity every commit in
# this repository already carries, so the packages say what the history says.
set(CPACK_PACKAGE_CONTACT "The Mole Authors <lukasz@gajos.pl>")
# /usr rather than the /usr/local a local `make install` uses: a distribution
# package that put itself in /usr/local would be in the place reserved for what
# the administrator installed by hand.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR "DEB;RPM")

# Qt loads three kinds of thing that nothing links, so no tool can derive them from
# the binaries: the SQL driver, the QML modules the interface imports, and the media
# backend that actually decodes. Every other dependency is derived below rather than
# typed.
set(MOLE_RUNTIME_QML_MODULES
    qml6-module-qtquick
    qml6-module-qtquick-controls
    qml6-module-qtquick-layouts
    qml6-module-qtquick-templates
    qml6-module-qtquick-window
    qml6-module-qtquick-shapes
    qml6-module-qtqml-workerscript)
if(MOLE_HAVE_MULTIMEDIA)
    list(APPEND MOLE_RUNTIME_QML_MODULES qml6-module-qtmultimedia)
endif()
list(JOIN MOLE_RUNTIME_QML_MODULES ", " MOLE_DEB_QML_DEPENDS)

# --- .deb -------------------------------------------------------------------

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT) # mole_<version>_amd64.deb, as policy has it
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
# Derived, not typed. dpkg-shlibdeps reads the binary and every plugin, asks the
# system which package each library came from, and writes exactly what this build
# linked -- including whichever optional libraries it found. A list typed once
# declares too little, so the package installs and then will not start, or too
# much, so it refuses to install on a system that has everything it needs.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, ${MOLE_DEB_QML_DEPENDS}")

# The third kind. Qt Multimedia is a front end and picks a backend at run time; on
# Debian and Ubuntu the platform default is GStreamer, whose decoders live in
# packages nothing here links. Measured on a clean ubuntu:24.04 with the .deb
# installed and nothing else -- MOLE-317:
#
#   as it installs today          none -- so videos are tried anyway and nothing decodes
#   with these two                MPEG4, H264, H265, VP8, Theora, WMV
#
# Recommends rather than Depends, because that is the strength the relationship has:
# apt installs recommends by default, so the ordinary install gets video, while a
# minimal system without `universe` enabled -- where gstreamer1.0-libav lives -- can
# still install the package and browse files. A missing decoder must not be an
# install failure.
if(MOLE_HAVE_MULTIMEDIA)
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gstreamer1.0-plugins-good, gstreamer1.0-libav")
endif()

# --- .rpm -------------------------------------------------------------------

set(CPACK_RPM_FILE_NAME RPM-DEFAULT) # mole-<version>-1.x86_64.rpm
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/File")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# rpmbuild derives the linked libraries itself, the same way and for the same
# reason as shlibdeps above; this is the pair it cannot see. One package rather
# than seven, because Fedora ships the QML modules and the SQL driver inside
# qt6-qtdeclarative and qt6-qtbase-gui rather than splitting them the way Debian
# Built and installed on the family it is for, in a container, by
# `make rpm` -- see scripts/package-rpm.sh, and the release workflow, which
# installs the result in a fresh image that did not build it. It said
# "unverified on an RPM system: nothing in this repository can build or install
# one" for long after both of those existed.
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtdeclarative, qt6-qtbase-gui")
# Directories the distribution owns. Without this the package claims /usr/share
# and its subdirectories and conflicts with everything.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps
    /usr/share/doc)

include(CPack)

endif() # MOLE_CORE_ONLY: there is nothing to package
