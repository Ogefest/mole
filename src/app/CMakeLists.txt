qt_add_executable(mole
    main.cpp
    SessionLog.cpp
    SessionLog.h
)

# Exported symbols, so the backtrace written when the program crashes carries
# function names rather than bare addresses.
set_target_properties(mole PROPERTIES ENABLE_EXPORTS ON)

# QML lives in its own module so the views are addressable by url, which is how
# a feature points the shell at its tab body. RESOURCE_PREFIX is set explicitly
# because its default changed after Qt 6.4.
# Listed rather than globbed: a glob is evaluated when CMake runs, so a new file
# would not trigger a reconfigure and would go missing until somebody noticed.
#
# The check underneath is what makes the list safe. The tests build their own
# copy of this directory with a glob, at the same resource prefix, so a file can
# exist, be exercised by fifty-seven green tests, and still be absent from the
# application -- which is exactly how ui/UnlockBand.qml shipped a window that
# could not load. An omission is an ordinary slip; a build where the slip is
# invisible to every test is the thing worth fixing.
set(MOLE_QML_FILES
    ui/Main.qml
    ui/AppMenu.qml
    ui/CommandPalette.qml
    ui/CompressDialog.qml
    ui/TargetList.qml
    ui/ConfirmButtons.qml
    ui/Sidebar.qml
    ui/UnlockBand.qml
    ui/TaskStrip.qml
    ui/FilePane.qml
    ui/BrowserView.qml
    ui/LiveSearchView.qml
    ui/IndexSearchView.qml
    ui/SearchResultList.qml
    ui/TextPreview.qml
    ui/PreviewView.qml
    ui/ImagePreview.qml
    ui/PdfPreview.qml
    ui/TablePreview.qml
    ui/FileInfoPreview.qml
    ui/AnalysisView.qml
    ui/AutomationView.qml
    ui/AlertsView.qml
    ui/ViewerKeys.qml
    ui/ReportsView.qml
    ui/DataGrid.qml
    ui/SqlitePreview.qml
    ui/ParquetPreview.qml
    ui/FileSetsView.qml
    ui/BulkRenameView.qml
    ui/DuplicatesView.qml
    ui/SyncView.qml
    ui/TerminalPanel.qml
    ui/DrivesDialog.qml
)

# Configure fails rather than the window failing to open. CONFIGURE_DEPENDS so
# that adding a file re-runs this check without anybody having to remember.
file(GLOB MOLE_QML_ON_DISK CONFIGURE_DEPENDS
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "ui/*.qml")
set(MOLE_QML_MISSING "")
foreach(qml IN LISTS MOLE_QML_ON_DISK)
    if(NOT qml IN_LIST MOLE_QML_FILES)
        list(APPEND MOLE_QML_MISSING ${qml})
    endif()
endforeach()
if(MOLE_QML_MISSING)
    message(FATAL_ERROR
        "These QML files are in src/app/ui but not in MOLE_QML_FILES, so the "
        "application would be built without them and the window would fail to "
        "load:\n  ${MOLE_QML_MISSING}\n"
        "Add them to the list in src/app/CMakeLists.txt.")
endif()

qt_add_qml_module(mole
    URI Mole
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES ${MOLE_QML_FILES}
)

target_link_libraries(mole
    PRIVATE
        mole_builtin
        Qt6::Quick
        Qt6::QuickControls2
        mole_flags)

set_target_properties(mole PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MOLE_HAVE_ARCHIVE_PLUGIN)
    add_dependencies(mole mole_plugin_archive)
endif()
