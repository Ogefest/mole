# Built-in features are compiled into the application but registered through
# the same PluginRegistry a third-party plugin uses.
add_library(mole_builtin STATIC
    builtin/BuiltinPlugin.h
    builtin/BuiltinPlugin.cpp
    builtin/BrowserFeature.h
    builtin/BrowserFeature.cpp
    builtin/SearchFeatures.h
    builtin/SearchFeatures.cpp
    builtin/PreviewFeature.h
    builtin/PreviewFeature.cpp
    builtin/AnalysisFeature.h
    builtin/AnalysisFeature.cpp
    builtin/AnalysisJob.h
    builtin/AnalysisJob.cpp
    builtin/AutomationFeature.h
    builtin/AutomationFeature.cpp
    builtin/AlertsFeature.h
    builtin/AlertsFeature.cpp
    builtin/ReportsFeature.h
    builtin/ReportsFeature.cpp
    builtin/FileSetsFeature.h
    builtin/FileSetsFeature.cpp
    builtin/BulkRenameFeature.h
    builtin/BulkRenameFeature.cpp
    builtin/DuplicatesFeature.h
    builtin/DuplicatesFeature.cpp
    builtin/SyncFeature.h
    builtin/SyncFeature.cpp

    rclone/RcloneLibrary.h
    rclone/RcloneLibrary.cpp
    rclone/RcloneFileSystem.h
    rclone/RcloneFileSystem.cpp
    rclone/RcloneFactory.h
    rclone/RcloneFactory.cpp
    builtin/previews/SyntaxHighlighter.h
    builtin/previews/SyntaxHighlighter.cpp
    builtin/previews/MarkdownStyle.h
    builtin/previews/MarkdownStyle.cpp
    builtin/previews/PreviewProviders.h
    builtin/previews/PreviewProviders.cpp
    builtin/previews/PdfPreview.h
    builtin/previews/PdfPreview.cpp
)

# Qt PDF renders the pages. Optional: without it the provider still compiles and
# still refuses every file, so a PDF falls through to the information viewer
# instead of opening a frame nothing can fill. See docs/adr/0004-pdf-previews.md.
find_package(Qt6 QUIET COMPONENTS Pdf)
if(Qt6Pdf_FOUND)
    target_link_libraries(mole_builtin PUBLIC Qt6::Pdf)
    target_compile_definitions(mole_builtin PUBLIC MOLE_HAVE_QTPDF)
else()
    message(STATUS
        "Qt6 Pdf not found -- PDFs will open in the information viewer. "
        "Install qt6-pdf-dev (Debian/Ubuntu) to render them.")
endif()

add_library(mole::builtin ALIAS mole_builtin)

target_include_directories(mole_builtin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Qt6::Quick only for QQuickTextDocument, which is how a syntax highlighter
# attaches to a QML TextArea. No window is created here.
target_link_libraries(mole_builtin
    PUBLIC mole_ui Qt6::Quick
    PRIVATE mole_flags)

# --- Archive support, built as a genuinely separate shared library ----------
#
# It is loaded at runtime through QPluginLoader like any community plugin.
# Keeping one shipped feature on that path is what stops the plugin API from
# quietly rotting.
find_package(LibArchive)

if(LibArchive_FOUND)
    # The backend is a normal library so the test suite can hold it to the same
    # conformance contract as every other filesystem. The loadable plugin below
    # is only the thin wrapper that publishes it.
    add_library(mole_archive_backend STATIC
        archive/ArchiveFileSystem.h
        archive/ArchiveFileSystem.cpp
    )
    target_include_directories(mole_archive_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(mole_archive_backend
        PUBLIC mole_core
        PRIVATE LibArchive::LibArchive mole_flags)

    qt_add_plugin(mole_plugin_archive
        SHARED
        CLASS_NAME mole::ArchivePlugin
    )

    # Qt 6.4's qt_add_plugin() ignores trailing source arguments, which yields a
    # library with no code and no metadata. Sources have to be added separately.
    target_sources(mole_plugin_archive PRIVATE archive/ArchivePlugin.cpp)

    target_include_directories(mole_plugin_archive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(mole_plugin_archive
        PRIVATE mole_sdk mole_archive_backend mole_flags)

    # Dropped next to the executable, where PluginManager looks first.
    set_target_properties(mole_plugin_archive PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")

    set(MOLE_HAVE_ARCHIVE_PLUGIN ON CACHE INTERNAL "")
else()
    # TODO: install libarchive-dev to enable browsing inside zip/tar/7z.
    message(WARNING
        "libarchive not found -- the archive plugin will not be built. "
        "Install libarchive-dev (Debian/Ubuntu) or libarchive (vcpkg) to enable it.")
    set(MOLE_HAVE_ARCHIVE_PLUGIN OFF CACHE INTERNAL "")
endif()

# librclone is rclone built as a shared library: no binary, no daemon, no child
# process. It is 115 MB of Go, so it is not vendored -- `make librclone` builds
# it from source, and without it the factory reports itself unavailable and the
# application runs exactly as before.
set(MOLE_LIBRCLONE_DIR "${CMAKE_SOURCE_DIR}/third_party/librclone")
if(EXISTS "${MOLE_LIBRCLONE_DIR}/librclone.so")
    target_compile_definitions(mole_builtin PRIVATE
        MOLE_LIBRCLONE_BUILD_PATH="${MOLE_LIBRCLONE_DIR}/librclone.so")
    message(STATUS "rclone drives: enabled")
else()
    message(STATUS "rclone drives: disabled (run `make librclone`)")
endif()
