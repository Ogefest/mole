cmake_minimum_required(VERSION 3.24)

project(Mole
    VERSION 0.1.0
    DESCRIPTION "Extensible cross-platform file manager"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MOLE_BUILD_TESTS "Build unit tests" ON)
option(MOLE_ENABLE_SANITIZERS "Enable ASan + UBSan" OFF)

# Qt 6.4 is the baseline (Ubuntu 24.04 ships 6.4.2). Do not use newer APIs
# without bumping this number first -- it is what keeps the port to
# Windows/macOS honest.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql Concurrent)

qt_standard_project_setup()

# Compile flags shared by every target in the project.
add_library(mole_flags INTERFACE)
target_compile_options(mole_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wnon-virtual-dtor>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)

if(MOLE_ENABLE_SANITIZERS)
    target_compile_options(mole_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mole_flags INTERFACE -fsanitize=address,undefined)
endif()

# Strictly layered, each depending only on the ones above it:
#   core -> sdk -> host -> ui -> builtin -> app
add_subdirectory(src/core)
add_subdirectory(src/sdk)
add_subdirectory(src/host)
add_subdirectory(src/ui)
add_subdirectory(src/plugins)
add_subdirectory(src/app)

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

install(TARGETS mole
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MOLE_HAVE_ARCHIVE_PLUGIN)
    install(TARGETS mole_plugin_archive
        LIBRARY DESTINATION ${MOLE_PLUGIN_INSTALL_DIR})
endif()

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
