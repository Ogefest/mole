# Shared test scaffolding: event-loop helpers, temp trees, fake plugins and the
# backend conformance suite. Test binaries link this instead of copy-pasting
# fixtures around.
add_library(mole_test_support STATIC
    support/TestSupport.h
    support/TestSupport.cpp
    support/FileSystemConformance.h
    support/FileSystemConformance.cpp
    support/FakePlugin.h
    support/MoleTestMain.h
)

# The QML-driving harness needs Quick and the application's QML; kept in its
# own target so the headless core tests do not pay for it.
add_library(mole_qml_harness STATIC
    support/QmlAppHarness.h
    support/QmlAppHarness.cpp
)
target_include_directories(mole_qml_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mole_qml_harness
    PUBLIC mole_builtin mole_test_support Qt6::Quick Qt6::QuickControls2 Qt6::Test
    PRIVATE mole_flags)

file(GLOB MOLE_APP_QML_FILES "${CMAKE_SOURCE_DIR}/src/app/ui/*.qml")
qt_add_resources(mole_qml_harness "harness_qml"
    PREFIX "/qt/qml/Mole"
    BASE "${CMAKE_SOURCE_DIR}/src/app"
    FILES ${MOLE_APP_QML_FILES})

target_include_directories(mole_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mole_test_support
    PUBLIC mole_core mole_sdk Qt6::Test
    PRIVATE mole_flags)

# Tests that load the archive plugin need to know where it was built.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")

# Tests that spin up a real QQuickWindow need the Qt scene-graph leak
# suppressions: Qt does not release its textures when a window is torn down in
# process, and there is no API to make it. Applied by name so a new QML test
# gets it by being listed here, rather than by someone remembering.
function(mole_mark_qml_test name)
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-qt.supp")
endfunction()

# One binary per unit under test: a crash in one suite cannot hide the others,
# and ctest -j runs them all in parallel.
function(mole_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${ARG_UNPARSED_ARGUMENTS})
    endif()

    qt_add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE mole_test_support ${ARG_LIBS} mole_flags)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180)
endfunction()

# --- core: no GUI, no plugins -----------------------------------------------
mole_add_test(tst_VfsUri            SOURCES core/tst_VfsUri.cpp)
mole_add_test(tst_LocalFileSystem   SOURCES core/tst_LocalFileSystem.cpp)
mole_add_test(tst_MemoryFileSystem  SOURCES core/tst_MemoryFileSystem.cpp)
mole_add_test(tst_VfsManager        SOURCES core/tst_VfsManager.cpp)
mole_add_test(tst_DirectoryWalker   SOURCES core/tst_DirectoryWalker.cpp)
mole_add_test(tst_TaskManager       SOURCES core/tst_TaskManager.cpp)
mole_add_test(tst_IndexDatabase     SOURCES core/tst_IndexDatabase.cpp)
mole_add_test(tst_ScanTask          SOURCES core/tst_ScanTask.cpp)
mole_add_test(tst_LiveSearchTask    SOURCES core/tst_LiveSearchTask.cpp)
mole_add_test(tst_EventBus          SOURCES core/tst_EventBus.cpp)
mole_add_test(tst_TransferTask      SOURCES core/tst_TransferTask.cpp)
mole_add_test(tst_FolderSizesTask   SOURCES core/tst_FolderSizesTask.cpp)
mole_add_test(tst_Preferences       SOURCES core/tst_Preferences.cpp)
mole_add_test(tst_ReadRangeTask     SOURCES core/tst_ReadRangeTask.cpp)
mole_add_test(tst_DelimitedText     SOURCES core/tst_DelimitedText.cpp)
mole_add_test(tst_DelimitedStore    SOURCES core/tst_DelimitedStore.cpp)
mole_add_test(tst_TableSources     SOURCES core/tst_TableSources.cpp)
mole_add_test(tst_RenamePlan       SOURCES core/tst_RenamePlan.cpp)
mole_add_test(tst_Duplicates       SOURCES core/tst_Duplicates.cpp)
mole_add_test(tst_Sync             SOURCES core/tst_Sync.cpp)
mole_add_test(tst_Terminal         SOURCES core/tst_Terminal.cpp)
mole_add_test(tst_SecretStore      SOURCES core/tst_SecretStore.cpp)
mole_add_test(tst_RemoteRegistry   SOURCES core/tst_RemoteRegistry.cpp)
mole_add_test(tst_Analysis          SOURCES core/tst_Analysis.cpp)
mole_add_test(tst_Scheduler         SOURCES core/tst_Scheduler.cpp)
mole_add_test(tst_Alerts            SOURCES core/tst_Alerts.cpp)

# --- host: the extension system ---------------------------------------------
mole_add_test(tst_PluginManager     SOURCES host/tst_PluginManager.cpp  LIBS mole_host)
mole_add_test(tst_ActionRegistry    SOURCES host/tst_ActionRegistry.cpp LIBS mole_host)

# --- ui: models and controllers ---------------------------------------------
mole_add_test(tst_FileListModel        SOURCES ui/tst_FileListModel.cpp        LIBS mole_ui)
mole_add_test(tst_MountListModel       SOURCES ui/tst_MountListModel.cpp       LIBS mole_ui)
mole_add_test(tst_TaskListModel        SOURCES ui/tst_TaskListModel.cpp        LIBS mole_ui)
mole_add_test(tst_TabsModel            SOURCES ui/tst_TabsModel.cpp            LIBS mole_ui)
mole_add_test(tst_BrowserPaneController SOURCES ui/tst_BrowserPaneController.cpp LIBS mole_ui)
mole_add_test(tst_FileLauncher         SOURCES ui/tst_FileLauncher.cpp         LIBS mole_ui)
mole_add_test(tst_Bookmarks            SOURCES ui/tst_Bookmarks.cpp            LIBS mole_ui)
mole_add_test(tst_CommandPalette       SOURCES ui/tst_CommandPalette.cpp       LIBS mole_ui)

# --- the assembled application ----------------------------------------------
mole_add_test(tst_AppIntegration    SOURCES app/tst_AppIntegration.cpp   LIBS mole_builtin)
mole_add_test(tst_Session           SOURCES app/tst_Session.cpp          LIBS mole_builtin)
mole_add_test(tst_Preview           SOURCES app/tst_Preview.cpp          LIBS mole_builtin)
mole_add_test(tst_AnalysisTab       SOURCES app/tst_AnalysisTab.cpp     LIBS mole_builtin)
mole_add_test(tst_Automation        SOURCES app/tst_Automation.cpp      LIBS mole_builtin)
mole_add_test(tst_FileSets          SOURCES app/tst_FileSets.cpp        LIBS mole_builtin)

# Drives the real QML end to end and photographs each verified state.
# Screenshots land in MOLE_SCREENSHOT_DIR when it is set; `make screenshots`
# does that. No X server, no window manager, no input lottery.
mole_add_test(tst_Walkthrough       SOURCES app/tst_Walkthrough.cpp     LIBS mole_qml_harness)

# The session log lives in the application target rather than a library, so the
# test compiles the one file it is about. Exported symbols, so the backtrace it
# checks for carries names.
mole_add_test(tst_SessionLog        SOURCES app/tst_SessionLog.cpp ../src/app/SessionLog.cpp)
set_target_properties(tst_SessionLog PROPERTIES ENABLE_EXPORTS ON)

# Drives the real QML with real key events. The model tests prove a cursor
# moves; only this proves a keystroke ever reaches it.
mole_add_test(tst_KeyboardNavigation SOURCES app/tst_KeyboardNavigation.cpp
                                    LIBS mole_builtin Qt6::Quick Qt6::QuickControls2)

# The QML lives in the application target, so the test gets its own copy of the
# same files at the same resource path rather than duplicating the markup.
file(GLOB MOLE_APP_QML "${CMAKE_SOURCE_DIR}/src/app/ui/*.qml")
qt_add_resources(tst_KeyboardNavigation "app_qml"
    PREFIX "/qt/qml/Mole"
    BASE "${CMAKE_SOURCE_DIR}/src/app"
    FILES ${MOLE_APP_QML})

# After both are registered: set_tests_properties needs the test to exist.
mole_mark_qml_test(tst_KeyboardNavigation)
mole_mark_qml_test(tst_Walkthrough)
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    add_dependencies(tst_AppIntegration mole_plugin_archive)
    add_dependencies(tst_PluginManager mole_plugin_archive)
endif()

# --- plugins ------------------------------------------------------------------
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    mole_add_test(tst_Rclone            SOURCES plugins/tst_Rclone.cpp LIBS mole_builtin)

    mole_add_test(tst_ArchiveFileSystem SOURCES plugins/tst_ArchiveFileSystem.cpp
                                       LIBS mole_archive_backend)
else()
    message(STATUS "Archive tests skipped: the archive plugin was not built")
endif()
