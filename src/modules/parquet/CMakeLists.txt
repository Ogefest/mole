# The Parquet module: Mole's Parquet reader, built against Arrow as a shared
# object the application loads at run time, rather than a library it links.
#
# **Why a module.** A binary linked to Arrow cannot start where Arrow is missing,
# so Parquet used to be all or nothing per artefact -- and the one .deb is for
# three distributions of which only Ubuntu 26.04 packages Arrow. As a module,
# Parquet is something a Mole finds or does not find: ParquetTable in core loads
# `mole-parquet.so` the first time a Parquet file is asked about, and without it
# the preview falls back to the file-information view. For the .deb the module is
# built in an ubuntu:26.04 container by scripts/package-parquet-module.sh and put
# into the package built on 24.04. Every other artefact builds it here, beside
# the application, so there is one way Parquet is reached everywhere. See
# ADR-0106 and MOLE-459.
#
# Built in a core-only configuration too, which is how the container builds it:
# it needs Qt Core and Arrow and none of Mole's own libraries.
mole_optional_dependency(PARQUET
    SUMMARY "Parquet preview"
    PACKAGE Arrow Parquet
    VERSION_FROM ARROW_VERSION
    FOUND "enabled (Arrow @VERSION@, as a module loaded at run time)"
    MISSING "disabled (install libarrow-dev and libparquet-dev)"
    CACHE_PREFIX MOLE_PARQUET)

if(MOLE_HAVE_PARQUET)
    add_library(mole_parquet MODULE ParquetModule.cpp)
    # mole-parquet.so, not libmole_parquet.so: it is found by that name and
    # loaded by path, never linked.
    set_target_properties(mole_parquet PROPERTIES
        PREFIX ""
        OUTPUT_NAME mole-parquet
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
        # Only the four entry points are the module's interface; everything else
        # stays inside it, so its copy of anything cannot be bound to the
        # application's by accident.
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # The interface header lives in core, and that is all of core this needs.
    target_include_directories(mole_parquet PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(mole_parquet PRIVATE
        Qt6::Core Arrow::arrow_shared Parquet::parquet_shared mole_flags)
    # Arrow names a parameter `signals`, and Qt's macro of that name expands to
    # `public:`. The module has no signals and no slots, so Qt's keywords are off
    # here rather than kept out of the way by include order, which clang-format
    # rearranges.
    target_compile_definitions(mole_parquet PRIVATE QT_NO_KEYWORDS)

    if(NOT MOLE_CORE_ONLY)
        include(GNUInstallDirs)
        install(TARGETS mole_parquet
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/mole/modules)
    endif()
endif()
