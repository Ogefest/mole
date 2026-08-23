cmake_minimum_required(VERSION 3.24)

project(Mole
    VERSION 0.1.0
    DESCRIPTION "Extensible cross-platform file manager"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

# Qt 6.4 is the baseline (Ubuntu 24.04 ships 6.4.2). Do not use newer APIs
# without bumping this number first -- it is what keeps the port to
# Windows/macOS honest.
if(MOLE_CORE_ONLY)
    # Everything here is in qtbase, which is the whole point -- see the option above.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Sql Concurrent)
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql Concurrent)
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
add_subdirectory(src/tools)
# The two that need Qml, Quick, Pdf or Multimedia, and so the two a qtbase-only
# Qt cannot build. Everything above needs nothing outside qtbase.
if(NOT MOLE_CORE_ONLY)
    add_subdirectory(src/plugins)
    add_subdirectory(src/app)
endif()

if(MOLE_BUILD_TESTS)
    # Network is for the tests alone -- Mole itself talks to servers through
    # libcurl and never links it. It is what lets a suite stand up a server that
    # answers wrongly on purpose, which is the only way most of those answers can
    # be met at all. See tests/support/ScriptedHttpServer.h.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test Network)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- installing
#
# Layout:
#   <prefix>/bin/mole
#   <prefix>/lib/mole/plugins/*.so
#   <prefix>/share/applications, <prefix>/share/icons/...
#
# PluginManager resolves the plugin directory relative to the executable, so
# the same build runs from any prefix without being rebuilt.
include(GNUInstallDirs)

set(MOLE_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/mole/plugins")

if(NOT MOLE_CORE_ONLY)
install(TARGETS mole mole-tasks
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MOLE_HAVE_ARCHIVE_PLUGIN)
    install(TARGETS mole_plugin_archive
        LIBRARY DESTINATION ${MOLE_PLUGIN_INSTALL_DIR})
endif()
endif() # MOLE_CORE_ONLY: there is no application to install

# Licence paperwork travels with every install: Qt's LGPL requires the notice
# and its text to reach whoever receives the binary.
install(FILES LICENSE NOTICE THIRD-PARTY-NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/mole)
install(DIRECTORY licenses
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/mole)

install(FILES packaging/mole.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES packaging/mole.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
