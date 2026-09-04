# One shape for every optional library, because there were six.
#
# **Eleven libraries in this build are optional**, and every one of them followed
# the same rule with a different hand: find it quietly, compile behind a define,
# and report itself unavailable at run time rather than failing to build. What
# differed was everything else. One find was not QUIET and warned where the rest
# reported; two printed nothing at all on success, so the workflows that grep the
# summary could not show them and the release had to refuse on *not-found* text;
# three passed `target_link_directories()` and two did not, so a library in a
# prefix the linker does not search worked for some and not for others; one define
# was PRIVATE where the rest were PUBLIC; and one had a `MOLE_WITH_` switch for a
# reason that applies to all of them.
#
# **The message shape is an interface.** `scripts/feature-summary.sh` holds a
# release build to these lines, the release workflow refuses an artefact that is
# missing one, and three workflows print them so that whoever reads a red run can
# see what the machine did not have. A library added in the wrong shape is
# invisible to all of that -- which is what had happened to Qt Pdf and Qt
# Multimedia. So the line is written here, once, and every row gets one whether it
# was found or not. See MOLE-390.
#
# Usage:
#
#   mole_optional_dependency(PARQUET
#       SUMMARY   "Parquet preview"           # the words before the colon
#       PACKAGE   Arrow Parquet                # find_package, QUIET, all required
#       VERSION   "${ARROW_VERSION}"           # what to say when it is found
#       FOUND     "enabled (Arrow @VERSION@)"  # or a whole sentence
#       MISSING   "disabled (install libarrow-dev and libparquet-dev)"
#       TARGET    mole_core
#       SCOPE     PUBLIC
#       DEFINE    MOLE_HAVE_PARQUET
#       LINK      Arrow::arrow_shared Parquet::parquet_shared)
#
# The name is the row: it sets `MOLE_HAVE_PARQUET` in the caller's scope whatever
# happens, so a caller can gate a whole target on it rather than only a define.
#
# One of PACKAGE, QT_COMPONENT or PKG_CONFIG says how to look. SWITCH names an
# option that has to be ON for the search to happen at all; CACHE_PREFIX exports
# what was found to the cache, for a directory that is configured elsewhere and
# cannot see these variables -- the test tree is the one that needs it.

include_guard(GLOBAL)

function(mole_optional_dependency name)
    set(options "")
    set(single SUMMARY VERSION VERSION_FROM FOUND MISSING MISSING_EXTRA TARGET SCOPE LINK_SCOPE
        DEFINE SWITCH CACHE_PREFIX EXTRA_CONDITION)
    set(multi PACKAGE QT_COMPONENT PKG_CONFIG LINK INCLUDE LINK_DIRS)
    cmake_parse_arguments(ARG "${options}" "${single}" "${multi}" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "mole_optional_dependency(${name}): unknown argument(s) ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_SUMMARY)
        message(FATAL_ERROR "mole_optional_dependency(${name}): SUMMARY is what the reader sees, and there is none")
    endif()
    if(NOT ARG_SCOPE)
        set(ARG_SCOPE PUBLIC)
    endif()
    # SCOPE is the define's and LINK_SCOPE is the library's, because they are not
    # the same question and two rows need them apart: Qt6::Pdf and
    # Qt6::Multimedia are linked PUBLIC so that their headers reach whoever
    # compiles against mole_builtin -- the test tree does -- while a library that
    # only this target calls is linked PRIVATE, which is the rest of them.
    if(NOT ARG_LINK_SCOPE)
        set(ARG_LINK_SCOPE PRIVATE)
    endif()

    set(found FALSE)
    set(detail "")

    # A switch that is off is a different answer from a library that is missing,
    # and it says so: "unavailable (install ...)" sends somebody to their package
    # manager for a decision this build already took.
    set(may_look TRUE)
    if(ARG_SWITCH AND NOT ${ARG_SWITCH})
        set(may_look FALSE)
    endif()

    if(may_look)
        if(ARG_PACKAGE)
            set(found TRUE)
            foreach(package IN LISTS ARG_PACKAGE)
                find_package(${package} QUIET)
                if(NOT ${package}_FOUND)
                    set(found FALSE)
                endif()
            endforeach()
        elseif(ARG_QT_COMPONENT)
            find_package(Qt6 QUIET COMPONENTS ${ARG_QT_COMPONENT})
            set(found TRUE)
            foreach(component IN LISTS ARG_QT_COMPONENT)
                if(NOT Qt6${component}_FOUND)
                    set(found FALSE)
                endif()
            endforeach()
        elseif(ARG_PKG_CONFIG)
            # pkg-config is itself optional: a machine without it has none of
            # these libraries as far as this build is concerned, which is the
            # honest answer rather than a configure error.
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(${name} QUIET ${ARG_PKG_CONFIG})
            endif()
            if(${name}_FOUND)
                set(found TRUE)
                # What pkg-config answered, for the caller to link with.
                if(NOT ARG_LINK)
                    set(ARG_LINK ${${name}_LIBRARIES})
                endif()
                if(NOT ARG_INCLUDE)
                    set(ARG_INCLUDE ${${name}_INCLUDE_DIRS})
                endif()
                if(NOT ARG_LINK_DIRS)
                    # Every row gets these, and three of eleven had them: a
                    # library in /usr/local/lib or a Homebrew prefix links for the
                    # rows that pass them and fails for the rows that do not.
                    set(ARG_LINK_DIRS ${${name}_LIBRARY_DIRS})
                endif()
                if(NOT ARG_VERSION)
                    set(ARG_VERSION "${${name}_VERSION}")
                endif()
            endif()
        else()
            message(FATAL_ERROR
                "mole_optional_dependency(${name}): nothing says how to look for it "
                "-- one of PACKAGE, QT_COMPONENT or PKG_CONFIG")
        endif()
    endif()

    # VERSION_FROM names a variable rather than holding a value, and it has to:
    # a version written as `VERSION "${ARROW_VERSION}"` at the call site is
    # expanded before this function has looked for anything, so every
    # find_package row printed an empty version. Which variable carries it is the
    # package's business -- ARROW_VERSION, OPENSSL_VERSION, CURL_VERSION_STRING,
    # Qt6Pdf_VERSION -- so the row names it and this reads it afterwards.
    if(ARG_VERSION_FROM AND NOT ARG_VERSION)
        set(ARG_VERSION "${${ARG_VERSION_FROM}}")
    endif()

    # A second question the search cannot answer. Qt Multimedia is the case: the
    # C++ library and the QML module `import QtMultimedia` resolves to are two
    # packages, either can be absent on its own, and there is no CMake component
    # for the second.
    set(extra_failed FALSE)
    if(found AND ARG_EXTRA_CONDITION AND NOT ${ARG_EXTRA_CONDITION})
        set(found FALSE)
        set(extra_failed TRUE)
    endif()

    if(found)
        set(detail "${ARG_FOUND}")
    elseif(NOT may_look AND ARG_SWITCH)
        set(detail "not built (${ARG_SWITCH} is OFF)")
    elseif(extra_failed AND ARG_MISSING_EXTRA)
        set(detail "${ARG_MISSING_EXTRA}")
    else()
        set(detail "${ARG_MISSING}")
    endif()
    string(REPLACE "@VERSION@" "${ARG_VERSION}" detail "${detail}")
    message(STATUS "${ARG_SUMMARY}: ${detail}")

    if(found AND ARG_TARGET)
        if(ARG_DEFINE)
            target_compile_definitions(${ARG_TARGET} ${ARG_SCOPE} ${ARG_DEFINE})
        endif()
        if(ARG_INCLUDE)
            target_include_directories(${ARG_TARGET} PRIVATE ${ARG_INCLUDE})
        endif()
        if(ARG_LINK_DIRS)
            target_link_directories(${ARG_TARGET} PRIVATE ${ARG_LINK_DIRS})
        endif()
        if(ARG_LINK)
            target_link_libraries(${ARG_TARGET} ${ARG_LINK_SCOPE} ${ARG_LINK})
        endif()
    endif()

    # For a directory configured elsewhere: find_package and pkg_check_modules
    # results do not cross between sibling directories, so the test tree cannot
    # see any of this without the cache.
    if(ARG_CACHE_PREFIX)
        set(${ARG_CACHE_PREFIX}_FOUND ${found} CACHE INTERNAL "")
        if(found)
            set(${ARG_CACHE_PREFIX}_INCLUDE_DIRS "${ARG_INCLUDE}" CACHE INTERNAL "")
            set(${ARG_CACHE_PREFIX}_LIBRARIES "${ARG_LINK}" CACHE INTERNAL "")
            set(${ARG_CACHE_PREFIX}_LIBRARY_DIRS "${ARG_LINK_DIRS}" CACHE INTERNAL "")
        endif()
    endif()

    set(MOLE_HAVE_${name} ${found} PARENT_SCOPE)
endfunction()
