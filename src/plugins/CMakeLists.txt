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
    builtin/IndexScanJob.h
    builtin/IndexScanJob.cpp
    builtin/AutomationFeature.h
    builtin/AutomationFeature.cpp
    builtin/AlertsFeature.h
    builtin/AlertsFeature.cpp
    builtin/ReportsFeature.h
    builtin/ReportsFeature.cpp
    builtin/IndexesFeature.h
    builtin/IndexesFeature.cpp
    builtin/TimeWords.h
    builtin/TimeWords.cpp
    builtin/FileSetsFeature.h
    builtin/FileSetsFeature.cpp
    builtin/BulkRenameFeature.h
    builtin/BulkRenameFeature.cpp
    builtin/DuplicatesFeature.h
    builtin/DuplicatesFeature.cpp
    builtin/SyncFeature.h
    builtin/SyncFeature.cpp

    builtin/previews/SyntaxHighlighter.h
    builtin/previews/SyntaxHighlighter.cpp
    builtin/previews/MarkdownStyle.h
    builtin/previews/MarkdownStyle.cpp
    builtin/previews/PreviewProviders.h
    builtin/previews/PreviewProviders.cpp
    builtin/previews/ReadImageHeaderTask.h
    builtin/previews/ReadImageHeaderTask.cpp
    builtin/previews/MetadataReaders.h
    builtin/previews/MetadataReaders.cpp
    builtin/previews/ImageMetadata.h
    builtin/previews/ImageMetadata.cpp
    builtin/previews/DocumentMetadata.h
    builtin/previews/DocumentMetadata.cpp
    builtin/previews/MediaMetadata.h
    builtin/previews/MediaMetadata.cpp
    builtin/previews/AudioMetadata.h
    builtin/previews/AudioMetadata.cpp
    builtin/previews/PdfPreview.h
    builtin/previews/PdfPreview.cpp
    builtin/previews/RenderPdfPageTask.h
    builtin/previews/RenderPdfPageTask.cpp
    builtin/previews/OpenPdfDocumentTask.h
    builtin/previews/OpenPdfDocumentTask.cpp
    builtin/previews/VideoPreview.h
    builtin/previews/VideoPreview.cpp

    builtin/thumbnails/ThumbnailLimits.h
    builtin/thumbnails/ImageThumbnailer.h
    builtin/thumbnails/ImageThumbnailer.cpp
    builtin/thumbnails/PdfThumbnailer.h
    builtin/thumbnails/PdfThumbnailer.cpp
    builtin/thumbnails/VideoThumbnailer.h
    builtin/thumbnails/VideoThumbnailer.cpp
)

# Qt PDF renders the pages. Optional: without it the provider still compiles and
# still refuses every file, so a PDF falls through to the information viewer
# instead of opening a frame nothing can fill. See docs/adr/0004-pdf-previews.md.
#
# It printed nothing when it was found, which is why this row was invisible: three
# workflows print the configure summary and the release refuses an artefact that
# is missing a line, so a row that only speaks when it is absent can only ever be
# checked as a *not-found* string. Every row says what it is now. See MOLE-390.
mole_optional_dependency(QTPDF
    SUMMARY "Document preview"
    QT_COMPONENT Pdf
    VERSION_FROM Qt6Pdf_VERSION
    FOUND "Qt Pdf @VERSION@"
    MISSING "information viewer (install qt6-pdf-dev to render PDFs)"
    TARGET mole_builtin
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_QTPDF
    LINK_SCOPE PUBLIC
    LINK Qt6::Pdf)

# Qt Multimedia plays the video. Optional in exactly the same shape as Qt PDF:
# without it the provider still compiles and still refuses every file, so a video
# falls through to the information viewer instead of opening a frame nothing can
# fill.
#
# Two halves, checked separately, because they are two packages and either can be
# missing on its own: the C++ library this compiles against, and the QML module
# `import QtMultimedia` resolves to at run time. There is no `MultimediaQuick`
# CMake component to ask for -- Debian ships the QML module without one -- so the
# module is looked for where Qt says its QML modules live.
set(MOLE_QTMULTIMEDIA_QML "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}/QtMultimedia/qmldir")
if(EXISTS "${MOLE_QTMULTIMEDIA_QML}")
    set(MOLE_QTMULTIMEDIA_QML_INSTALLED TRUE)
else()
    set(MOLE_QTMULTIMEDIA_QML_INSTALLED FALSE)
endif()
mole_optional_dependency(MULTIMEDIA
    SUMMARY "Video preview"
    QT_COMPONENT Multimedia
    VERSION_FROM Qt6Multimedia_VERSION
    EXTRA_CONDITION MOLE_QTMULTIMEDIA_QML_INSTALLED
    FOUND "Qt Multimedia @VERSION@"
    MISSING_EXTRA "information viewer (the library is installed and its QML module is not: install qml6-module-qtmultimedia)"
    MISSING "information viewer (install qt6-multimedia-dev and qml6-module-qtmultimedia to play videos)"
    TARGET mole_builtin
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_MULTIMEDIA
    LINK_SCOPE PUBLIC
    LINK Qt6::Multimedia)
# For the test suite, which asserts playback through the real player and needs the
# same enums. The same shape MOLE_HAVE_ARCHIVE_PLUGIN uses below.
set(MOLE_HAVE_MULTIMEDIA ${MOLE_HAVE_MULTIMEDIA} CACHE INTERNAL "")

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
#
# The only find here that was not QUIET, and the only one that warned rather than
# reported: a machine without libarchive got a CMake *warning* for a library the
# build is designed to do without, where the other ten printed a status line. See
# MOLE-390.
mole_optional_dependency(ARCHIVE
    SUMMARY "Archive drives"
    PACKAGE LibArchive
    VERSION_FROM LibArchive_VERSION
    FOUND "libarchive @VERSION@"
    MISSING "not built (install libarchive-dev to browse inside zip, tar and 7z)")

if(MOLE_HAVE_ARCHIVE)
    # The backend is a normal library so the test suite can hold it to the same
    # conformance contract as every other filesystem. The loadable plugin below
    # is only the thin wrapper that publishes it.
    add_library(mole_archive_backend STATIC
        archive/ArchiveFileSystem.h
        archive/ArchiveFileSystem.cpp
        archive/ZipPrefix.h
        archive/ZipPrefix.cpp
        archive/CompressTask.h
        archive/CompressTask.cpp
    )
    target_include_directories(mole_archive_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(mole_archive_backend
        PUBLIC mole_core
        PRIVATE LibArchive::LibArchive mole_flags)

    # CLASS_NAME is the *unqualified* name, and Qt is right to insist. It is only
    # ever used to build a `Q_IMPORT_PLUGIN(...)` for a static plugin, and what moc
    # exports for `mole::ArchivePlugin` is `qt_static_plugin_ArchivePlugin` -- a
    # qualified name could not name that symbol even if the macro accepted one. Qt
    # 6.4 stored whatever it was given, so `mole::ArchivePlugin` sat here unnoticed
    # until Qt 6.10 began validating it and refused to configure at all. See
    # MOLE-389.
    qt_add_plugin(mole_plugin_archive
        SHARED
        CLASS_NAME ArchivePlugin
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

    # Writing archives is an operation on the files in front of you rather than a
    # kind of drive, so the writer is linked where the operation lives -- with
    # AppController in mole_ui -- while the loadable plugin stays the thin wrapper
    # that publishes the filesystem. No layering is inverted: the backend sits on
    # mole_core like mole_ui does, and knows nothing about the interface.
    # See docs/adr/0007-writing-archives.md.
    target_link_libraries(mole_ui PUBLIC mole_archive_backend)
    target_compile_definitions(mole_ui PUBLIC MOLE_HAVE_ARCHIVE)

    set(MOLE_HAVE_ARCHIVE_PLUGIN ON CACHE INTERNAL "")
else()
    set(MOLE_HAVE_ARCHIVE_PLUGIN OFF CACHE INTERNAL "")
endif()

# --- Standard network drives: sftp, ftp, s3, webdav -------------------------
#
# One libcurl covers all four protocols, and its blocking interface is what
# IFileSystem's threading contract asks for. See
# docs/adr/0011-network-drives-without-rclone.md.
# Both are asked for here rather than relied on from src/core: find_package
# results do not cross between sibling directories, so a variable set there is
# simply empty by the time this runs.
mole_optional_dependency(NETWORK
    SUMMARY "Network drives"
    PACKAGE CURL OpenSSL
    VERSION_FROM CURL_VERSION_STRING
    FOUND "sftp, ftp, s3, webdav (libcurl @VERSION@)"
    MISSING "not built (install libcurl4-openssl-dev and libssl-dev)")

if(MOLE_HAVE_NETWORK)
    # A plain library first, so the test suite can hold each backend to the same
    # conformance contract as local disk. The loadable plugin below is only the
    # thin wrapper that publishes them.
    add_library(mole_network_backends STATIC
        network/CurlTransport.h
        network/CurlTransport.cpp
        network/TransferStreams.h
        network/TransferStreams.cpp
        network/UnixListing.h
        network/UnixListing.cpp
        network/SftpFileSystem.h
        network/SftpFileSystem.cpp
        network/S3Signer.h
        network/S3Signer.cpp
        network/S3Listing.h
        network/S3Listing.cpp
        network/S3FileSystem.h
        network/S3FileSystem.cpp
        network/FtpFileSystem.h
        network/FtpFileSystem.cpp
        network/WebdavListing.h
        network/WebdavListing.cpp
        network/WebdavFileSystem.h
        network/WebdavFileSystem.cpp
    )

    # SMB through Samba's own client library, and optional like every other
    # backend dependency: a build without it ships the other drives and says so,
    # rather than failing to configure. See ADR-0048.
    # MOLE_WITH_SMB is off for the self-contained artefacts and on everywhere
    # else, and the helper says which of the two absences this is: a switch that
    # is off is a decision this build took, where a missing library is something
    # somebody could install. See ADR-0094 and the option in the top-level
    # CMakeLists.txt.
    mole_optional_dependency(SMB
        SUMMARY "Windows shares"
        PKG_CONFIG smbclient
        SWITCH MOLE_WITH_SMB
        FOUND "smb (libsmbclient @VERSION@)"
        MISSING "unavailable (install libsmbclient-dev)"
        TARGET mole_network_backends
        SCOPE PUBLIC
        DEFINE MOLE_HAVE_SMB)
    if(MOLE_HAVE_SMB)
        target_sources(mole_network_backends PRIVATE
            network/SmbFileSystem.h
            network/SmbFileSystem.cpp)
    endif()

    # NFS through libnfs, a userspace client -- no mount, no root. Optional the
    # same way, and for the same reason. See ADR-0050.
    mole_optional_dependency(NFS
        SUMMARY "NFS exports"
        PKG_CONFIG libnfs
        FOUND "nfs (libnfs @VERSION@)"
        MISSING "unavailable (install libnfs-dev)"
        TARGET mole_network_backends
        SCOPE PUBLIC
        DEFINE MOLE_HAVE_NFS)
    if(MOLE_HAVE_NFS)
        target_sources(mole_network_backends PRIVATE
            network/NfsFileSystem.h
            network/NfsFileSystem.cpp)
    endif()
    target_include_directories(mole_network_backends PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(mole_network_backends
        PUBLIC mole_core
        PRIVATE CURL::libcurl OpenSSL::Crypto mole_flags)

    qt_add_plugin(mole_plugin_network
        SHARED
        # Unqualified, for the reason beside the archive plugin above.
        CLASS_NAME NetworkPlugin
    )

    # Qt 6.4's qt_add_plugin() ignores trailing source arguments, exactly as it
    # does for the archive plugin.
    target_sources(mole_plugin_network PRIVATE network/NetworkPlugin.cpp)

    target_include_directories(mole_plugin_network PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(mole_plugin_network
        PRIVATE mole_sdk mole_network_backends mole_flags)

    set_target_properties(mole_plugin_network PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")

    set(MOLE_HAVE_NETWORK_PLUGIN ON CACHE INTERNAL "")
else()
    set(MOLE_HAVE_NETWORK_PLUGIN OFF CACHE INTERNAL "")
endif()

# Network drives used to be rclone, linked straight into mole_builtin as 115 MB of
# Go. They are now the loadable plugin above. See
# docs/adr/0011-network-drives-without-rclone.md.
