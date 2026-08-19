# Shared test scaffolding: event-loop helpers, temp trees, fake plugins and the
# backend conformance suite. Test binaries link this instead of copy-pasting
# fixtures around.
add_library(mole_test_support STATIC
    support/TestSupport.h
    support/TestSupport.cpp
    support/FileSystemConformance.h
    support/FileSystemConformance.cpp
    support/FaultyFileSystem.h
    support/FaultyFileSystem.cpp
    support/FakePlugin.h
    support/MoleTestMain.h
    support/TestbedControl.h
    support/TestbedControl.cpp
    support/ScriptedHttpServer.h
    support/ScriptedHttpServer.cpp
    support/TableFixtures.h
    support/TableFixtures.cpp
    support/Victim.h
    support/Victim.cpp
    support/GitFixture.h
    support/GitFixture.cpp
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
    PUBLIC mole_core mole_sdk Qt6::Test Qt6::Network Qt6::Sql Qt6::Gui
    PRIVATE mole_flags)

# The git fixtures build their repositories through libgit2 itself, so the support
# library links it as well as core does. Core exports where it found it, because
# this directory is configured in a scope of its own.
if(MOLE_GIT2_FOUND)
    target_include_directories(mole_test_support PRIVATE ${MOLE_GIT2_INCLUDE_DIRS})
    target_link_directories(mole_test_support PRIVATE ${MOLE_GIT2_LIBRARY_DIRS})
    target_link_libraries(mole_test_support PRIVATE ${MOLE_GIT2_LIBRARIES})
endif()

# Where the user guide keeps its pictures, so a test can hold the rule that every
# feature has one -- see everyFeatureAndPreviewHasAPictureInTheGuide.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_GUIDE_IMAGE_DIR="${CMAKE_SOURCE_DIR}/docs/guide/images")

# Tests that load the archive plugin need to know where it was built.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")

# The console runner lands next to the application rather than next to the test
# binaries, and one test starts it for real.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_TASKS_BINARY="${CMAKE_BINARY_DIR}/mole-tasks")

# Every suite gets the leak suppressions in lsan.supp rather than a list of suites
# opting in by name. Two reasons. Every entry there is scoped to a third-party module
# -- Qt's scene graph, QPdfDocument, libgit2 -- so a leak in our own code still fails
# the build; the opt-in was protecting nothing. And now that git state is read on
# every navigation, the suites that drive libgit2 are most of them, so a by-name list
# is a trap: it goes red for whoever adds the next suite, for a reason they did not
# cause. See lsan.supp for what each entry is and why it cannot be freed here.

function(mole_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${ARG_UNPARSED_ARGUMENTS})
    endif()

    qt_add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE mole_test_support ${ARG_LIBS} mole_flags)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
        TIMEOUT 180)
endfunction()

# --- core: no GUI, no plugins -----------------------------------------------
mole_add_test(tst_VfsUri            SOURCES core/tst_VfsUri.cpp)
mole_add_test(tst_LocalFileSystem   SOURCES core/tst_LocalFileSystem.cpp)

# What a process killed outright leaves behind. It starts a second copy of
# itself and sends it SIGKILL, which is the one failure that cannot be faked
# from inside a process that intends to keep running.
mole_add_test(tst_SweepLeftovers   SOURCES core/tst_SweepLeftovers.cpp)
mole_add_test(tst_KilledOutright    SOURCES core/tst_KilledOutright.cpp LIBS Qt6::Sql)
mole_add_test(tst_MemoryFileSystem  SOURCES core/tst_MemoryFileSystem.cpp)

# The fault injector is a test tool, and one that miscounts bytes would make
# every suite built on it green for the wrong reason.
mole_add_test(tst_FaultyFileSystem  SOURCES support/tst_FaultyFileSystem.cpp)
mole_add_test(tst_VfsManager        SOURCES core/tst_VfsManager.cpp)
mole_add_test(tst_DirectoryWalker   SOURCES core/tst_DirectoryWalker.cpp)
mole_add_test(tst_TaskManager       SOURCES core/tst_TaskManager.cpp)
mole_add_test(tst_DriveCheckTask    SOURCES core/tst_DriveCheckTask.cpp)
mole_add_test(tst_IndexDatabase     SOURCES core/tst_IndexDatabase.cpp LIBS Qt6::Sql)
mole_add_test(tst_ScanTask          SOURCES core/tst_ScanTask.cpp)
mole_add_test(tst_LiveSearchTask    SOURCES core/tst_LiveSearchTask.cpp)
mole_add_test(tst_SearchQuery       SOURCES core/tst_SearchQuery.cpp)

# Looking inside the files: the window loop, the overlap, the encodings and the
# ceiling are each a thing that can be wrong on its own.
mole_add_test(tst_ContentSearch     SOURCES core/tst_ContentSearch.cpp)

# The line above the form. Its own binary because a parser is worth breaking on
# its own rather than through a controller.
mole_add_test(tst_QueryLine         SOURCES core/tst_QueryLine.cpp)
mole_add_test(tst_EventBus          SOURCES core/tst_EventBus.cpp)
mole_add_test(tst_TransferTask      SOURCES core/tst_TransferTask.cpp)

# A file arrives whole, or the copy says it did not: every pair of the backends
# that need no server, at every boundary size the copy loop has.
mole_add_test(tst_FilesArriveWhole  SOURCES core/tst_FilesArriveWhole.cpp)

# What happens when the machine runs out of something.
mole_add_test(tst_RunningOut        SOURCES core/tst_RunningOut.cpp)

# Names people really have, and shapes a filesystem really holds.
mole_add_test(tst_AwkwardNames      SOURCES core/tst_AwkwardNames.cpp)

# Several jobs over the same files at once: what must never appear is a file
# that is half of one and half of the other.
mole_add_test(tst_ManyAtOnce        SOURCES core/tst_ManyAtOnce.cpp)

# Cancellation, aimed at a stage rather than at a moment: the walk, the arrival
# check, a move's delete pass, and the queue before any of it starts.
mole_add_test(tst_Cancellation      SOURCES core/tst_Cancellation.cpp)

# Delete, which has nothing to compare afterwards: what else went with it.
mole_add_test(tst_Delete            SOURCES core/tst_Delete.cpp)

# The one operation that deletes something: every stage of a move interrupted,
# and the source still there afterwards.
mole_add_test(tst_MoveIsPermanent   SOURCES core/tst_MoveIsPermanent.cpp)

# The same task, everything going wrong: the copy loop's guards, one hostile
# scenario each, through the fault injector.
mole_add_test(tst_TransferTaskUnderFault SOURCES core/tst_TransferTaskUnderFault.cpp)
mole_add_test(tst_FolderSizesTask   SOURCES core/tst_FolderSizesTask.cpp)
mole_add_test(tst_Preferences       SOURCES core/tst_Preferences.cpp)
mole_add_test(tst_ReadRangeTask     SOURCES core/tst_ReadRangeTask.cpp)

# What a file is, from a page of it: the sniff every preview goes through when
# the name had nothing to say.
mole_add_test(tst_FileType          SOURCES core/tst_FileType.cpp)
mole_add_test(tst_DelimitedText     SOURCES core/tst_DelimitedText.cpp)
mole_add_test(tst_DelimitedStore    SOURCES core/tst_DelimitedStore.cpp)
mole_add_test(tst_TableSources     SOURCES core/tst_TableSources.cpp)
mole_add_test(tst_RenamePlan       SOURCES core/tst_RenamePlan.cpp)

# Carrying a batch of renames out, where the order is the whole problem.
mole_add_test(tst_RenameTask       SOURCES core/tst_RenameTask.cpp)
mole_add_test(tst_Duplicates       SOURCES core/tst_Duplicates.cpp)
mole_add_test(tst_Sync             SOURCES core/tst_Sync.cpp)
mole_add_test(tst_SyncTaskUnderFault SOURCES core/tst_SyncTaskUnderFault.cpp)
mole_add_test(tst_Terminal         SOURCES core/tst_Terminal.cpp)
mole_add_test(tst_SecretStore      SOURCES core/tst_SecretStore.cpp)
mole_add_test(tst_RemoteRegistry   SOURCES core/tst_RemoteRegistry.cpp)

# What git says about a folder. Fixture repositories are built through libgit2 in
# a temporary directory, so this needs no installed git and no network; without
# libgit2 every case skips itself and says why.
mole_add_test(tst_Repository        SOURCES core/tst_Repository.cpp)
mole_add_test(tst_Analysis          SOURCES core/tst_Analysis.cpp)
mole_add_test(tst_Scheduler         SOURCES core/tst_Scheduler.cpp)
mole_add_test(tst_Alerts            SOURCES core/tst_Alerts.cpp)

# --- tools: the console runner ----------------------------------------------
#
# Driven in-process: a runner tested only by starting a process gives up its own
# failure messages, and its exit codes are half of what it is for. One test in
# there does start the binary, because "runs with no display" is a claim about a
# process and cannot be checked from inside one.
mole_add_test(tst_MoleTasks         SOURCES tools/tst_MoleTasks.cpp LIBS mole_tasks_cli)
add_dependencies(tst_MoleTasks mole-tasks)

# --- host: the extension system ---------------------------------------------
mole_add_test(tst_PluginManager     SOURCES host/tst_PluginManager.cpp  LIBS mole_host)
mole_add_test(tst_ActionRegistry    SOURCES host/tst_ActionRegistry.cpp LIBS mole_host)

# --- ui: models and controllers ---------------------------------------------
mole_add_test(tst_FileListModel        SOURCES ui/tst_FileListModel.cpp        LIBS mole_ui)
mole_add_test(tst_DriveListModel       SOURCES ui/tst_DriveListModel.cpp       LIBS mole_ui)
mole_add_test(tst_TableModel           SOURCES ui/tst_TableModel.cpp           LIBS mole_ui)
mole_add_test(tst_TaskListModel        SOURCES ui/tst_TaskListModel.cpp        LIBS mole_ui)
mole_add_test(tst_TabsModel            SOURCES ui/tst_TabsModel.cpp            LIBS mole_ui)
mole_add_test(tst_BrowserPaneController SOURCES ui/tst_BrowserPaneController.cpp LIBS mole_ui)
mole_add_test(tst_FileLauncher         SOURCES ui/tst_FileLauncher.cpp         LIBS mole_ui)

# What a selection looks like once it has left the window. No window and no
# pointer in here: the step that hands the payload to the platform is a hook,
# and this replaces it with a recorder.
mole_add_test(tst_DragSource           SOURCES ui/tst_DragSource.cpp           LIBS mole_ui)
mole_add_test(tst_Bookmarks            SOURCES ui/tst_Bookmarks.cpp            LIBS mole_ui)
mole_add_test(tst_CommandPalette       SOURCES ui/tst_CommandPalette.cpp       LIBS mole_ui)

# --- the assembled application ----------------------------------------------
mole_add_test(tst_AppIntegration    SOURCES app/tst_AppIntegration.cpp   LIBS mole_builtin)

# A folder with an indexed subtree in it, answered by both engines at once. Its
# own binary because it fills an in-memory drive and interferes with it between
# the scan and the search, which is state the integration suite would trip over.
mole_add_test(tst_MixedSearch       SOURCES app/tst_MixedSearch.cpp      LIBS mole_builtin)
mole_add_test(tst_Session           SOURCES app/tst_Session.cpp          LIBS mole_builtin)
mole_add_test(tst_Preview           SOURCES app/tst_Preview.cpp          LIBS mole_builtin)

# What a photograph says about itself: an EXIF walk over bytes the test wrote,
# including the offsets a hostile file would write.
mole_add_test(tst_ImageMetadata     SOURCES plugins/tst_ImageMetadata.cpp LIBS mole_builtin)

# Who wrote a document: a zip built by hand, walked from its front, and a PDF
# written by the test. Includes the hostile container and the hostile XML.
mole_add_test(tst_DocumentMetadata  SOURCES plugins/tst_DocumentMetadata.cpp LIBS mole_builtin)

# What a video says about itself, over containers built box by box -- including
# the box that claims to be four gigabytes long inside a 64 kB buffer.
mole_add_test(tst_MediaMetadata     SOURCES plugins/tst_MediaMetadata.cpp LIBS mole_builtin)

# The tags inside an audio file: four formats, and the frame that claims to be
# longer than the block it is in.
mole_add_test(tst_AudioMetadata     SOURCES plugins/tst_AudioMetadata.cpp LIBS mole_builtin)
mole_add_test(tst_AnalysisTab       SOURCES app/tst_AnalysisTab.cpp     LIBS mole_builtin)
mole_add_test(tst_Automation        SOURCES app/tst_Automation.cpp      LIBS mole_builtin)
mole_add_test(tst_FileSets          SOURCES app/tst_FileSets.cpp        LIBS mole_builtin)

# Choosing what to keep, which is the half of a duplicate scan that can lose a
# file: no rule may ever propose deleting every copy of anything.
mole_add_test(tst_DuplicatesTab     SOURCES app/tst_DuplicatesTab.cpp   LIBS mole_qml_harness mole_builtin)

# Drives the real QML end to end and photographs each verified state.
# Screenshots land in MOLE_SCREENSHOT_DIR when it is set; `make screenshots`
# does that. No X server, no window manager, no input lottery.
mole_add_test(tst_Walkthrough       SOURCES app/tst_Walkthrough.cpp     LIBS mole_qml_harness)

# The drive rows in the sidebar. Their own binary rather than more of the
# walkthrough: this one configures drives and connects them, which is state the
# walkthrough's narrative would have to step around.
mole_add_test(tst_Sidebar           SOURCES app/tst_Sidebar.cpp         LIBS mole_qml_harness)

# Dragging rows out of a listing, through the real window. Its own binary
# because it holds a pointer button down across assertions and ticks rows, which
# is state the walkthrough's narrative would have to step around.
mole_add_test(tst_Dragging          SOURCES app/tst_Dragging.cpp         LIBS mole_qml_harness mole_builtin)

# What a tab says about where it came from. Its own binary for the same reason
# as the sidebar's: it opens and closes tabs, which is state a narrative test
# would have to step around.
mole_add_test(tst_TabStrip          SOURCES app/tst_TabStrip.cpp        LIBS mole_qml_harness mole_builtin)

# The band above a listing. Its own binary for the same reason again: it builds
# git repositories in the fixture and navigates between them, and it measures how
# tall the listing is, which is a claim about a folder that is not a checkout.
mole_add_test(tst_RepositoryBand    SOURCES app/tst_RepositoryBand.cpp  LIBS mole_qml_harness mole_builtin)

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

if(MOLE_HAVE_ARCHIVE_PLUGIN)
    add_dependencies(tst_AppIntegration mole_plugin_archive)
    add_dependencies(tst_PluginManager mole_plugin_archive)
    # A mounted archive is the read-only drive in Mole, and what a read-only pane
    # does with a drop is one of the things dragging has to answer for.
    add_dependencies(tst_Dragging mole_plugin_archive)
endif()

# These load the shipped plugins out of the build tree, so the plugins have to
# exist before they run: the drives dialog has nothing to offer without them.
if(MOLE_HAVE_NETWORK_PLUGIN)
    add_dependencies(tst_FileSets mole_plugin_network)
    add_dependencies(tst_Walkthrough mole_plugin_network)
    add_dependencies(tst_AppIntegration mole_plugin_network)
endif()

# --- plugins ------------------------------------------------------------------
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    mole_add_test(tst_ArchiveFileSystem SOURCES plugins/tst_ArchiveFileSystem.cpp
                                       LIBS mole_archive_backend)

    mole_add_test(tst_CompressTask      SOURCES plugins/tst_CompressTask.cpp
                                       LIBS mole_archive_backend)
else()
    message(STATUS "Archive tests skipped: the archive plugin was not built")
endif()

if(MOLE_HAVE_NETWORK_PLUGIN)
    # The parser tests are offline and always run. The backend tests reach a real
    # server and skip themselves when no account is in the environment, so the
    # suite is green on a machine that has none -- see ADR-0011.
    mole_add_test(tst_UnixListing      SOURCES plugins/tst_UnixListing.cpp
                                       LIBS mole_network_backends)

    # libcurl directly, because these seed their fixtures through a channel that
    # is not the backend under test.
    find_package(CURL QUIET)
    mole_add_test(tst_S3Signer         SOURCES plugins/tst_S3Signer.cpp
                                       LIBS mole_network_backends)

    # What the transport makes of a finished transfer, including the one thing a
    # backend cannot work out for itself: whether the file it was handed is whole.
    # Offline -- it judges responses, it does not fetch them.
    mole_add_test(tst_CurlTransport    SOURCES plugins/tst_CurlTransport.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    # The read stream that makes a file larger than the local disk copyable.
    # Offline: it is driven by a fake server in memory, which is also the only
    # way to test what happens when a transfer dies half way through.
    mole_add_test(tst_StreamingDownload SOURCES plugins/tst_StreamingDownload.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    # The write streams, and the step that gives an upload its real name only
    # once every byte of it has arrived. Offline: the interesting cases are a
    # send that fails and a stream that is abandoned, neither of which a
    # behaving server produces on request.
    mole_add_test(tst_StreamingUpload  SOURCES plugins/tst_StreamingUpload.cpp
                                       LIBS mole_network_backends mole_core CURL::libcurl)

    # Servers that answer wrongly on purpose. Offline, and it has to be: a
    # server that is behaving cannot be asked for a 411, a Content-Length that
    # does not match the body, or a listing cut off half way through -- and
    # those are the answers that lose data.
    mole_add_test(tst_ServersThatMisbehave SOURCES plugins/tst_ServersThatMisbehave.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    mole_add_test(tst_SftpFileSystem   SOURCES plugins/tst_SftpFileSystem.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    mole_add_test(tst_S3FileSystem     SOURCES plugins/tst_S3FileSystem.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    mole_add_test(tst_FtpFileSystem    SOURCES plugins/tst_FtpFileSystem.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    mole_add_test(tst_WebdavFileSystem SOURCES plugins/tst_WebdavFileSystem.cpp
                                       LIBS mole_network_backends CURL::libcurl)

    # The scale tier: gigabytes, against a real machine, with peak scratch
    # space, memory and file descriptors asserted rather than hoped for. Not
    # part of `make test` -- `make test-heavy` runs it, and every case skips
    # itself with a reason when there is no server or no room. Its own
    # directory because it is a tier rather than a unit: it tests no single
    # class, it tests what happens at a size.
    mole_add_test(tst_HeavyTransfers   SOURCES scale/tst_HeavyTransfers.cpp scale/HeavyPayload.cpp
                                       LIBS mole_network_backends CURL::libcurl)
    # An hour is not enough for 10 GB over a slow link, and a tier that times
    # out is a tier nobody trusts.
    # Two clocks, and both have to be told. CTest's TIMEOUT bounds the binary;
    # QtTest has a watchdog of its own -- five minutes per test function -- and
    # it is the one that fires first here. A read from a server that re-keys
    # inside the span pays a stall-guard wait at every re-key point (ADR-0013's
    # amendment), so a gibibyte legitimately takes about ten minutes. Left at the
    # default it looks like a hang, which is exactly what this tier exists to
    # tell apart from slowness.
    set_tests_properties(tst_HeavyTransfers PROPERTIES TIMEOUT 7200 LABELS heavy
                         ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=3600000")

    # The same transfers with the server being attacked while they run. Needs
    # the control channel as well as an account, and skips itself with the
    # reason when either is missing.
    mole_add_test(tst_Interference     SOURCES scale/tst_Interference.cpp scale/HeavyPayload.cpp
                                       LIBS mole_network_backends CURL::libcurl)
    set_tests_properties(tst_Interference PROPERTIES TIMEOUT 3600 LABELS heavy)
else()
    message(STATUS "Network drive tests skipped: the network plugin was not built")
endif()
