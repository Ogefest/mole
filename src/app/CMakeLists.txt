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
qt_add_qml_module(mole
    URI Mole
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        ui/Main.qml
        ui/AppMenu.qml
        ui/CommandPalette.qml
        ui/Sidebar.qml
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
