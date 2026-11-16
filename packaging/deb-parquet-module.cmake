# Puts the Parquet module into the .deb, with what it links as Recommends.
#
# **Why after cpack rather than through an install rule.** The module is built in
# an ubuntu:26.04 container against that distribution's Arrow (see
# scripts/package-parquet-module.sh), and the .deb is built on 24.04, which has no
# Arrow at all. Installed the ordinary way, dpkg-shlibdeps would read the module
# with the rest, fail to find libarrow.so on the machine building the package, and
# -- had it found one -- put Arrow in Depends, so the package would stop installing
# on the two distributions that have none. So the module is added to the finished
# package, out of shlibdeps' reach, and what it links becomes Recommends: apt
# installs those by default where they exist, and skips them without an error where
# they do not. See ADR-0106 and MOLE-459.
#
# **Derived, not typed.** The packages come from mole-parquet.recommends, which the
# container writes from the module's own NEEDED entries; anything the .deb already
# depends on (libc6, libstdc++6, the Qt core library) is left out, and what is left
# is Arrow -- libarrow2300 and libparquet2300 today, whatever the next soname is
# tomorrow.
#
# Run by cpack as a CPACK_POST_BUILD_SCRIPTS step, after
# packaging/deb-qt-alternatives.cmake. Without CPACK_MOLE_PARQUET_MODULE, or for a
# file that is not a .deb, it does nothing. tests/scripts/tst_Packages.sh drives it
# directly:
#
#   cmake -DCPACK_PACKAGE_FILES=<file.deb> -DCPACK_MOLE_PARQUET_MODULE=<module.so>
#         -DCPACK_MOLE_MODULE_DIR=usr/lib/mole/modules -P packaging/deb-parquet-module.cmake

cmake_minimum_required(VERSION 3.19)

if(NOT CPACK_MOLE_PARQUET_MODULE)
    return()
endif()
if(NOT EXISTS "${CPACK_MOLE_PARQUET_MODULE}")
    message(FATAL_ERROR "the Parquet module was promised at ${CPACK_MOLE_PARQUET_MODULE} and is not there")
endif()
if(NOT CPACK_MOLE_MODULE_DIR)
    message(FATAL_ERROR "CPACK_MOLE_MODULE_DIR says nothing about where the module goes in the package")
endif()

get_filename_component(module_dir "${CPACK_MOLE_PARQUET_MODULE}" DIRECTORY)
set(recommends_file "${module_dir}/mole-parquet.recommends")
if(NOT EXISTS "${recommends_file}")
    message(FATAL_ERROR "no ${recommends_file} beside the module, so what it links cannot be recommended")
endif()
file(STRINGS "${recommends_file}" linked)

foreach(package IN LISTS CPACK_PACKAGE_FILES)
    if(NOT package MATCHES "\\.deb$")
        continue()
    endif()

    find_program(DPKG_DEB dpkg-deb REQUIRED)
    get_filename_component(where "${package}" DIRECTORY)
    set(unpacked "${where}/parquet-module")
    file(REMOVE_RECURSE "${unpacked}")
    execute_process(COMMAND "${DPKG_DEB}" -R "${package}" "${unpacked}"
        RESULT_VARIABLE status ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "cannot unpack ${package} to add the Parquet module: ${error}")
    endif()

    # The module itself, where ParquetModule looks for it beside the installed binary.
    file(MAKE_DIRECTORY "${unpacked}/${CPACK_MOLE_MODULE_DIR}")
    file(COPY_FILE "${CPACK_MOLE_PARQUET_MODULE}" "${unpacked}/${CPACK_MOLE_MODULE_DIR}/mole-parquet.so")
    file(CHMOD "${unpacked}/${CPACK_MOLE_MODULE_DIR}/mole-parquet.so"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    file(MD5 "${unpacked}/${CPACK_MOLE_MODULE_DIR}/mole-parquet.so" md5)
    file(APPEND "${unpacked}/DEBIAN/md5sums" "${md5}  ${CPACK_MOLE_MODULE_DIR}/mole-parquet.so\n")
    file(SIZE "${CPACK_MOLE_PARQUET_MODULE}" bytes)
    math(EXPR kib "(${bytes} + 1023) / 1024")

    file(STRINGS "${unpacked}/DEBIAN/control" lines)
    set(depends "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^Depends: (.*)$")
            set(depends "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    # What the module links and the package does not already depend on. A name is
    # "already there" when it appears in Depends as a whole package name; the Qt
    # core library is named there as libqt6core6t64, which is also its name on
    # 26.04, so it falls out here with libc6 and libstdc++6.
    set(wanted "")
    foreach(name IN LISTS linked)
        string(STRIP "${name}" name)
        if(name STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE "([.+])" "\\\\\\1" quoted "${name}")
        if(NOT depends MATCHES "(^|[ ,|])${quoted}( |,|$)")
            list(APPEND wanted "${name}")
        endif()
    endforeach()
    list(JOIN wanted ", " wanted_text)

    set(control "")
    set(recommended FALSE)
    foreach(line IN LISTS lines)
        if(line MATCHES "^Recommends: (.*)$" AND wanted_text)
            set(line "Recommends: ${CMAKE_MATCH_1}, ${wanted_text}")
            set(recommended TRUE)
        elseif(line MATCHES "^Installed-Size: ([0-9]+)$")
            math(EXPR size "${CMAKE_MATCH_1} + ${kib}")
            set(line "Installed-Size: ${size}")
        endif()
        string(APPEND control "${line}\n")
    endforeach()
    if(wanted_text AND NOT recommended)
        string(APPEND control "Recommends: ${wanted_text}\n")
    endif()
    file(WRITE "${unpacked}/DEBIAN/control" "${control}")

    execute_process(COMMAND "${DPKG_DEB}" --root-owner-group -Zxz -b "${unpacked}" "${package}"
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
    file(REMOVE_RECURSE "${unpacked}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "cannot pack ${package} again after adding the Parquet module: ${error}")
    endif()
    message(STATUS "Parquet module added to ${package}; recommends ${wanted_text}")
endforeach()
