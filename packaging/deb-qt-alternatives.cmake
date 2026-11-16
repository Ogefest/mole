# Gives every Qt `t64` name in a .deb's Depends its unsuffixed alternative.
#
# **Why.** The .deb is built on Ubuntu 24.04 and dpkg-shlibdeps names what that
# machine calls each library. For most of Qt that is also what Debian 13 and Ubuntu
# 26.04 call it -- but Debian's Qt packaging dropped the `t64` suffix from some of
# its libraries (libqt6gui6, libqt6network6, libqt6sql6, libqt6concurrent6) while
# keeping it on others, and Ubuntu 26.04 took Debian's names. The file inside is
# the same `libQt6Gui.so.6` and Qt keeps binary compatibility within a major
# version, so the difference is only a name, and an alternative is enough:
#
#   libqt6gui6t64 (>= 6.4.0)   ->   libqt6gui6t64 (>= 6.4.0) | libqt6gui6 (>= 6.4.0)
#
# **A rule, not a list.** Any Qt library shlibdeps names gets one, so the next one
# added to the build is covered without anybody remembering. libqt6core6t64 exists
# under that name everywhere and gains an alternative that is never needed, which
# costs nothing.
#
# **Where it runs.** CPACK_DEBIAN_PACKAGE_SHLIBDEPS offers no filter on what it
# derives, so this is a CPACK_POST_BUILD_SCRIPTS step: the package CPack wrote is
# unpacked, its control file rewritten, and it is packed again with dpkg-deb. Run by
# cpack for every generator, and a no-op on anything that is not a .deb. See
# MOLE-458.
#
# Also usable on its own, which is how tests/scripts/tst_Packages.sh drives it:
#
#   cmake -DCPACK_PACKAGE_FILES=<file.deb> -P packaging/deb-qt-alternatives.cmake
#   cmake -DMOLE_DEPENDS="<a Depends value>" -P packaging/deb-qt-alternatives.cmake

cmake_minimum_required(VERSION 3.19)

# The rewrite itself, on a Depends value. Kept apart from the packing so it can be
# asked about without building a package.
function(mole_qt_alternatives depends out)
    string(REPLACE "," ";" items "${depends}")
    set(result "")
    foreach(item IN LISTS items)
        string(STRIP "${item}" item)
        if(item STREQUAL "")
            continue()
        endif()
        # Only a plain `libqt6<x>6t64 [(op version)]`: an item that already offers an
        # alternative was written by somebody on purpose and is left as it is.
        if(NOT item MATCHES "\\|"
           AND item MATCHES "^(libqt6[a-z0-9-]*[0-9])t64( \\([^)]*\\))?$")
            set(item "${item} | ${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
        endif()
        list(APPEND result "${item}")
    endforeach()
    list(JOIN result ", " joined)
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()

if(DEFINED MOLE_DEPENDS)
    mole_qt_alternatives("${MOLE_DEPENDS}" rewritten)
    message("${rewritten}")
    return()
endif()

foreach(package IN LISTS CPACK_PACKAGE_FILES)
    if(NOT package MATCHES "\\.deb$")
        continue()
    endif()

    find_program(DPKG_DEB dpkg-deb REQUIRED)
    get_filename_component(where "${package}" DIRECTORY)
    set(unpacked "${where}/qt-alternatives")
    file(REMOVE_RECURSE "${unpacked}")

    execute_process(COMMAND "${DPKG_DEB}" -R "${package}" "${unpacked}"
        RESULT_VARIABLE status ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "cannot unpack ${package} to give Qt its alternatives: ${error}")
    endif()

    file(STRINGS "${unpacked}/DEBIAN/control" lines)
    set(control "")
    set(changed FALSE)
    foreach(line IN LISTS lines)
        if(line MATCHES "^Depends: (.*)$")
            mole_qt_alternatives("${CMAKE_MATCH_1}" rewritten)
            set(line "Depends: ${rewritten}")
            set(changed TRUE)
        endif()
        string(APPEND control "${line}\n")
    endforeach()
    if(NOT changed)
        message(FATAL_ERROR "${package} has no Depends line, so dpkg-shlibdeps did not run")
    endif()
    file(WRITE "${unpacked}/DEBIAN/control" "${control}")

    # --root-owner-group: every file in the package belongs to root, which is what
    # CPack wrote and what the unpacking by an ordinary user would otherwise lose.
    execute_process(COMMAND "${DPKG_DEB}" --root-owner-group -Zxz -b "${unpacked}" "${package}"
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
    file(REMOVE_RECURSE "${unpacked}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "cannot pack ${package} again after giving Qt its alternatives: ${error}")
    endif()
    message(STATUS "Qt alternatives written into ${package}")
endforeach()
