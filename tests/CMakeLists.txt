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
    support/SqliteFaults.h
    support/SqliteFaults.cpp
    support/OfferingFileSystem.h
    support/OfferingFileSystem.cpp
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
# own target so the headless core tests do not pay for it -- and skipped whole in
# a core-only build, which has no Quick to link.
if(NOT MOLE_CORE_ONLY)
add_library(mole_qml_harness STATIC
    support/QmlAppHarness.h
    support/QmlAppHarness.cpp
)
target_include_directories(mole_qml_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mole_qml_harness
    PUBLIC mole_builtin mole_shell mole_test_support Qt6::Quick Qt6::QuickControls2 Qt6::Test
    PRIVATE mole_flags)

file(GLOB MOLE_APP_QML_FILES "${CMAKE_SOURCE_DIR}/src/app/ui/*.qml")
qt_add_resources(mole_qml_harness "harness_qml"
    PREFIX "/qt/qml/Mole"
    BASE "${CMAKE_SOURCE_DIR}/src/app"
    FILES ${MOLE_APP_QML_FILES})

endif() # MOLE_CORE_ONLY

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

# Where the application's QML lives, so a test can hold a rule about it that is
# cheaper to state than to enforce by review -- see tst_QmlConventions.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_QML_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/app/ui")

# And where the layers behind it live, so a rule about the shell covers the C++
# as well as the markup -- see nothingInTheShellNamesADriveOrAFilesystem.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_SHELL_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")

# And this directory, because a rule read from the source has to cover the fakes
# as well: FaultyFileSystem wraps a drive the way the shipped wrappers do and is
# held to the same forwarding -- see tst_WrappedDriveAnswers.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The top of the checkout, so a test can hold a promise the documentation makes
# against the code that has to keep it -- see readmeStatesWhatLeavesTheMachine.
target_compile_definitions(mole_test_support
    PUBLIC MOLE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

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

# A test that is a shell script rather than a C++ binary.
#
# `mole_add_test` above compiles and links; the scripts that build and damage the
# test environment are shell, and until MOLE-233 there was no path in this
# directory that ran one at all. That gap cost nine days of an NFS share exported
# read-write to a whole network, by way of a guard that had never once held --
# nothing here could have caught it, because nothing here ran the file.
#
# Bash by name rather than the executable bit, so the test runs the same whether
# or not the checkout preserved the mode. See tests/support/shelltest.sh for the
# stub `ssh` these use to provision nothing.
find_program(BASH_PROGRAM bash)

function(mole_add_shell_test name)
    cmake_parse_arguments(ARG "" "SCRIPT" "" ${ARGN})
    add_test(NAME ${name}
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SCRIPT})
    # No offscreen platform and no leak suppressions: there is no Qt here. The
    # timeout is short because these touch nothing outside a temp directory --
    # one that runs for a minute is one that found a machine to talk to.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60 LABELS shell)
endfunction()

if(BASH_PROGRAM)
    mole_add_shell_test(tst_TestbedProvisioning
                        SCRIPT scripts/tst_TestbedProvisioning.sh)
    mole_add_shell_test(tst_ShellScripts
                        SCRIPT scripts/tst_ShellScripts.sh)
    # The same idea for the C++ suites: rules that can be held by reading them,
    # so a new suite is covered by existing. See MOLE-273.
    mole_add_shell_test(tst_TestSuiteRules
                        SCRIPT scripts/tst_TestSuiteRules.sh)
    mole_add_shell_test(tst_MoleControl
                        SCRIPT scripts/tst_MoleControl.sh)
    # The changelog is the release notes, so its shape is checked here rather than
    # discovered by a release workflow. See ADR-0080.
    mole_add_shell_test(tst_Changelog
                        SCRIPT scripts/tst_Changelog.sh)
    # The version is written in one place and this holds everything that reads it to
    # that place, including the binary -- which is why this one is told where the
    # application is. A core-only build has no application, and the case says it
    # skipped rather than passing quietly. See MOLE-117.
    mole_add_shell_test(tst_Version
                        SCRIPT scripts/tst_Version.sh)
    # Cutting a release, driven against a repository the test builds: a work tree,
    # a bare repository standing in for the remote, and a Makefile carrying the
    # three targets the script asks of the project. So the commit, the annotated
    # tag and the push are real and none of it leaves the machine. See MOLE-118.
    mole_add_shell_test(tst_Release
                        SCRIPT scripts/tst_Release.sh)
    # And what the tag it pushes sets off. Structural, because whether a runner
    # publishes anything is a fact about GitHub -- what is held here is everything
    # that would make that run wrong. See MOLE-120.
    mole_add_shell_test(tst_ReleaseWorkflow
                        SCRIPT scripts/tst_ReleaseWorkflow.sh)
    # The .deb and the .rpm. Building one takes a release build and installing one
    # takes a container, so what is held here is the part that goes stale in
    # silence: the dependencies nothing links and so nothing derives. See MOLE-121.
    mole_add_shell_test(tst_Packages
                        SCRIPT scripts/tst_Packages.sh)
    # The self-contained bundle, and the same kind of silence one layer over: which
    # Qt plugin trees it carries. A tree left out of the bundler's list is a feature
    # the artefact has not got, and every other check still passes. See MOLE-317.
    mole_add_shell_test(tst_Bundle
                        SCRIPT scripts/tst_Bundle.sh)
    # The guard on the Qt LGPL conditions, and whether it can fail for the reason it
    # exists. It needs a real binary to build an artefact around: the first two
    # questions it asks are about one. See MOLE-298.
    mole_add_shell_test(tst_LicenceCheck
                        SCRIPT scripts/tst_LicenceCheck.sh)
    # And the check that starts an artefact for real, which exists because every
    # other one answers without a platform plugin. See MOLE-300.
    mole_add_shell_test(tst_StartCheck
                        SCRIPT scripts/tst_StartCheck.sh)
    # The release notes, taken out of the changelog rather than written again. Both
    # ends of the file and both ways a block can be empty. See MOLE-123.
    mole_add_shell_test(tst_ChangelogBlock
                        SCRIPT scripts/tst_ChangelogBlock.sh)
    # Every workflow, held to the two things all of them need -- it parses, and
    # something starts it -- and the second family held to what makes its answer
    # worth having: an account that is not root, and no locale. See MOLE-297.
    mole_add_shell_test(tst_Workflows
                        SCRIPT scripts/tst_Workflows.sh)
    # Every shell test above, run once more with a documented command's variables in
    # the environment. `make release VERSION=x` exports that assignment to
    # everything it runs, including the suite the gate runs before it cuts -- which
    # is how five cases in tst_Release stopped asserting what they were written to
    # assert and the release refused itself. A test whose answer depends on how it
    # was started cannot be trusted about anything. See MOLE-319.
    #
    # Longer than the sixty seconds the others get, because it is all of them in
    # series; and given MOLE_BINARY for the same reason two of them are, so the
    # inner runs assert rather than skip.
    mole_add_shell_test(tst_InheritedEnvironment
                        SCRIPT scripts/tst_InheritedEnvironment.sh)
    set_tests_properties(tst_InheritedEnvironment PROPERTIES TIMEOUT 300)

    if(NOT MOLE_CORE_ONLY)
        set_property(TEST tst_InheritedEnvironment APPEND PROPERTY
                     ENVIRONMENT "MOLE_BINARY=$<TARGET_FILE:mole>")
        set_property(TEST tst_LicenceCheck APPEND PROPERTY
                     ENVIRONMENT "MOLE_BINARY=$<TARGET_FILE:mole>")
    endif()
    if(NOT MOLE_CORE_ONLY)
        set_property(TEST tst_Version APPEND PROPERTY
                     ENVIRONMENT "MOLE_BINARY=$<TARGET_FILE:mole>")
    endif()
else()
    message(WARNING "Shell tests skipped: no bash found")
endif()

# The guide-picture comparator. Not a test: a small program `make screenshots-check`
# runs after taking the pictures twice, which says whether a regeneration would be
# reviewable. In this directory because it exists for the test suite's own output
# and links nothing of Mole's. See tests/tools/compare-shots.cpp and MOLE-255.
qt_add_executable(compare-shots tools/compare-shots.cpp)
target_link_libraries(compare-shots PRIVATE Qt6::Gui mole_flags)
set_target_properties(compare-shots PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --- core: no GUI, no plugins -----------------------------------------------
mole_add_test(tst_VfsUri            SOURCES core/tst_VfsUri.cpp)
mole_add_test(tst_SystemVolumes     SOURCES core/tst_SystemVolumes.cpp)
mole_add_test(tst_NameRules         SOURCES core/tst_NameRules.cpp)
mole_add_test(tst_QmlConventions    SOURCES app/tst_QmlConventions.cpp)
mole_add_test(tst_LocalFileSystem   SOURCES core/tst_LocalFileSystem.cpp)

# Earlier states of a file, where the filesystem already keeps them. The shape is
# a temporary tree, so the discovery walk and the per-folder union are held on any
# machine; the live case reads MOLE_TEST_SNAPSHOT_PATH and skips without it.
mole_add_test(tst_LocalSnapshots    SOURCES core/tst_LocalSnapshots.cpp)

# What a process killed outright leaves behind. It starts a second copy of
# itself and sends it SIGKILL, which is the one failure that cannot be faked
# from inside a process that intends to keep running.
mole_add_test(tst_SweepLeftovers   SOURCES core/tst_SweepLeftovers.cpp)
mole_add_test(tst_KilledOutright    SOURCES core/tst_KilledOutright.cpp LIBS Qt6::Sql)
mole_add_test(tst_MemoryFileSystem  SOURCES core/tst_MemoryFileSystem.cpp)

# What one drive can do and another cannot: the second tier of capability, the
# closed set of two outcomes, and the wrapper every mount goes through.
mole_add_test(tst_FileActions       SOURCES core/tst_FileActions.cpp)

# When a drive is asked what it can do where it was pointed, and the three
# states its answer has: unasked, answered, and could not say.
mole_add_test(tst_DriveProbe        SOURCES core/tst_DriveProbe.cpp)

# A uri that names an earlier state of a file, and the guard that keeps a drive
# which does not know what one is from answering with the current file.
mole_add_test(tst_FileVersions      SOURCES core/tst_FileVersions.cpp)

# The fault injector is a test tool, and one that miscounts bytes would make
# every suite built on it green for the wrong reason.
mole_add_test(tst_FaultyFileSystem  SOURCES support/tst_FaultyFileSystem.cpp)
# And the SQLite fault equipment, held to what it claims: a case that arranges a
# failure which does not happen asserts on whatever happened instead. See MOLE-293.
mole_add_test(tst_SqliteFaults      SOURCES support/tst_SqliteFaults.cpp)

# The drive that contributes actions. A fake that lies makes every suite built on
# it green for the wrong reason, so it is held to the same contract as a backend.
mole_add_test(tst_OfferingFileSystem SOURCES support/tst_OfferingFileSystem.cpp)
mole_add_test(tst_VfsManager        SOURCES core/tst_VfsManager.cpp)

# Every mount is behind two wrappers, and most of the suite holds a bare backend.
# This is the one that asks a wrapped drive the questions instead.
mole_add_test(tst_WrappedDriveAnswers SOURCES core/tst_WrappedDriveAnswers.cpp)
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
mole_add_test(tst_Staging            SOURCES core/tst_Staging.cpp)
mole_add_test(tst_Chain              SOURCES core/tst_Chain.cpp)
mole_add_test(tst_ChainTask          SOURCES core/tst_ChainTask.cpp)
mole_add_test(tst_ChainSources       SOURCES core/tst_ChainSources.cpp)

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
# Under no locale at all, which is where natural ordering broke.
#
# A C collator ignores numeric mode, so "file10" sorted before "file9" on any
# machine whose environment says nothing about language -- and the case that
# asserts natural ordering passed on every developer machine, because a desktop
# session sets LANG. Pinned here rather than left to the environment, so the
# check can fail for the reason it exists wherever it is run. See MOLE-297.
set_tests_properties(tst_FileListModel PROPERTIES ENVIRONMENT_MODIFICATION "LC_ALL=set:C;LANG=set:C")
mole_add_test(tst_DriveListModel       SOURCES ui/tst_DriveListModel.cpp       LIBS mole_ui)
mole_add_test(tst_TableModel           SOURCES ui/tst_TableModel.cpp           LIBS mole_ui)
mole_add_test(tst_TaskListModel        SOURCES ui/tst_TaskListModel.cpp        LIBS mole_ui)
mole_add_test(tst_TabsModel            SOURCES ui/tst_TabsModel.cpp            LIBS mole_ui)
mole_add_test(tst_BrowserPaneController SOURCES ui/tst_BrowserPaneController.cpp LIBS mole_ui)
# tst_Palette is a ui suite and is not here: it needs Qt6::Quick, so it sits below
# the core-only guard with the rest of what needs an engine. See MOLE-292.

# The headless half of the thumbnails: the url that keys one, the thread it is
# decoded on, and the cancellation that stops one nobody is waiting for.
mole_add_test(tst_ThumbnailSource      SOURCES ui/tst_ThumbnailSource.cpp      LIBS mole_ui)
mole_add_test(tst_ThumbnailCache       SOURCES ui/tst_ThumbnailCache.cpp       LIBS mole_ui)
mole_add_test(tst_FileLauncher         SOURCES ui/tst_FileLauncher.cpp         LIBS mole_ui)

# What a selection looks like once it has left the window. No window and no
# pointer in here: the step that hands the payload to the platform is a hook,
# and this replaces it with a recorder.
mole_add_test(tst_DragSource           SOURCES ui/tst_DragSource.cpp           LIBS mole_ui)
mole_add_test(tst_Bookmarks            SOURCES ui/tst_Bookmarks.cpp            LIBS mole_ui)
mole_add_test(tst_CommandPalette       SOURCES ui/tst_CommandPalette.cpp       LIBS mole_ui)

# The one request Mole makes on its own account, and everything it must not do
# while making it. Against a scripted local server: an offline machine, a 500, a
# body that stops half way and a connection that goes quiet all have to end in
# silence, and silence is the state that looks exactly like the check working.
# Nothing here reaches the internet -- see MOLE-324.
mole_add_test(tst_UpdateCheck          SOURCES ui/tst_UpdateCheck.cpp          LIBS mole_ui)

# --- the assembled application ----------------------------------------------
#
# From here down every suite needs the window, a plugin or a server, and each of
# those needs Qml, Quick, Pdf, Multimedia or libcurl. A core-only build has none
# of them -- see MOLE_CORE_ONLY in the top-level CMakeLists.
if(NOT MOLE_CORE_ONLY)

mole_add_test(tst_AppIntegration    SOURCES app/tst_AppIntegration.cpp   LIBS mole_builtin)

# A ui suite, kept down here rather than with the other ui ones because of what it
# links. Qt6::Quick because the one mistake worth testing for is a QML binding that
# never re-evaluates, and only a real engine can be asked whether it did; and
# mole_builtin because the contrast floor covers the source highlighter's nine
# colours, which live with the preview that uses them. A qtbase-only build has
# neither, and this being above the guard is what stopped `make tsan` configuring
# at all -- see MOLE-292 and the rule in tests/scripts/tst_TestSuiteRules.sh.
mole_add_test(tst_Palette           SOURCES ui/tst_Palette.cpp
                                    LIBS mole_ui mole_builtin Qt6::Quick)

# A folder with an indexed subtree in it, answered by both engines at once. Its
# own binary because it fills an in-memory drive and interferes with it between
# the scan and the search, which is state the integration suite would trip over.
mole_add_test(tst_MixedSearch       SOURCES app/tst_MixedSearch.cpp      LIBS mole_builtin)
mole_add_test(tst_Session           SOURCES app/tst_Session.cpp          LIBS mole_builtin)
# The preview tab, including what F3 does to a file compressed on its own. That
# last part needs a backend that can open one, so the archive library is linked in
# where the build has it and the cases that need it skip where it does not.
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    mole_add_test(tst_Preview       SOURCES app/tst_Preview.cpp
                                    LIBS mole_builtin mole_archive_backend)
    target_compile_definitions(tst_Preview PRIVATE MOLE_TEST_HAVE_ARCHIVE)
else()
    mole_add_test(tst_Preview       SOURCES app/tst_Preview.cpp          LIBS mole_builtin)
endif()

# What a photograph says about itself: an EXIF walk over bytes the test wrote,
# including the offsets a hostile file would write.
mole_add_test(tst_ImageMetadata     SOURCES plugins/tst_ImageMetadata.cpp LIBS mole_builtin)

# A picture of a picture: bounded, in the right orientation, and decoded scaled.
mole_add_test(tst_ImageThumbnailer  SOURCES plugins/tst_ImageThumbnailer.cpp LIBS mole_builtin)

# The other two kinds of picture: a document's first page and a frame from a
# video. Each skips itself where its dependency is not in the build.
mole_add_test(tst_MediaThumbnailers SOURCES plugins/tst_MediaThumbnailers.cpp LIBS mole_builtin)

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
# The nightly re-index, and whether the rule it runs from asks for the same scan
# the one that created it did. A stub metadata reader and a drive that mounts a
# fake container keep this off a real archive library. See MOLE-226.
mole_add_test(tst_IndexScanJob      SOURCES plugins/tst_IndexScanJob.cpp LIBS mole_builtin)

# The list of indexes: how old each one is, what kind of scan built it, and
# whether anything is keeping it fresh. Headless -- it drives the controller.
mole_add_test(tst_IndexesTab        SOURCES app/tst_IndexesTab.cpp     LIBS mole_builtin)

# The rule that the interface never reads the index on the thread that draws it,
# which is enforced by IndexDatabase rather than remembered -- see ADR-0066.
mole_add_test(tst_IndexOffTheDrawingThread SOURCES app/tst_IndexOffTheDrawingThread.cpp
                                           LIBS mole_builtin)

mole_add_test(tst_Automation        SOURCES app/tst_Automation.cpp      LIBS mole_builtin)
mole_add_test(tst_FileSets          SOURCES app/tst_FileSets.cpp        LIBS mole_builtin)

# The Sets tab under the keyboard: a set is somewhere to work, which means a
# cursor, arrows, Enter and F3 -- not a list to point at.
mole_add_test(tst_SetsTab           SOURCES app/tst_SetsTab.cpp         LIBS mole_qml_harness mole_builtin)

# Choosing what to keep, which is the half of a duplicate scan that can lose a
# file: no rule may ever propose deleting every copy of anything.
mole_add_test(tst_DuplicatesTab     SOURCES app/tst_DuplicatesTab.cpp   LIBS mole_qml_harness mole_builtin)
# The indexes tab with a window behind it. tst_IndexesTab is headless and covers what
# the controller answers; the fault in MOLE-265 was in the delegate the Repeat
# dropdown lives in, which a headless controller does not have.
mole_add_test(tst_IndexesView       SOURCES app/tst_IndexesView.cpp     LIBS mole_qml_harness mole_builtin)

# The notice a newer version gets, and the switch in Help that stops it being
# looked for. With a window behind it, because what is under test is what somebody
# sees: one toast, the version and nothing else, a button that opens the page the
# manifest named, and silence on every start where there is nothing new. The
# manifest comes from a scripted local server -- nothing here reaches the
# internet. See MOLE-325.
mole_add_test(tst_UpdateNotice      SOURCES app/tst_UpdateNotice.cpp    LIBS mole_qml_harness mole_builtin)
# Where the search form's criteria land on the screen. tst_MixedSearch is headless and
# covers what a search answers; MOLE-270 was six labels on a different row from the
# field they name, which only a window can see.
mole_add_test(tst_SearchForm        SOURCES app/tst_SearchForm.cpp      LIBS mole_qml_harness mole_builtin)

# The gallery and the thumbnail queue together: the bound, and what a folder
# somebody has left leaves behind. Neither is a claim about either alone.
mole_add_test(tst_Thumbnails        SOURCES app/tst_Thumbnails.cpp      LIBS mole_qml_harness mole_builtin)

# Drives the real QML end to end and photographs each verified state.
# Screenshots land in MOLE_SCREENSHOT_DIR when it is set; `make screenshots`
# does that. No X server, no window manager, no input lottery.
mole_add_test(tst_Walkthrough       SOURCES app/tst_Walkthrough.cpp     LIBS mole_qml_harness)
# Video playback is asserted through the real MediaPlayer, so the test needs the
# same enums the view is written against. Both cases are real: a build without Qt
# Multimedia skips those assertions, which is the other half of MOLE-37.
if(MOLE_HAVE_MULTIMEDIA)
    find_package(Qt6 QUIET COMPONENTS Multimedia)
    target_link_libraries(tst_Walkthrough PRIVATE Qt6::Multimedia)
    target_compile_definitions(tst_Walkthrough PRIVATE MOLE_TEST_HAVE_MULTIMEDIA)
endif()

# The drive rows in the sidebar. Their own binary rather than more of the
# walkthrough: this one configures drives and connects them, which is state the
# walkthrough's narrative would have to step around.
mole_add_test(tst_Sidebar           SOURCES app/tst_Sidebar.cpp         LIBS mole_qml_harness)

# Dragging rows out of a listing, through the real window. Its own binary
# because it holds a pointer button down across assertions and ticks rows, which
# is state the walkthrough's narrative would have to step around.
mole_add_test(tst_Dragging          SOURCES app/tst_Dragging.cpp         LIBS mole_qml_harness mole_builtin)

# The band above a listing. Its own binary for the same reason again: it builds
# git repositories in the fixture and navigates between them, and it measures how
# tall the listing is, which is a claim about a folder that is not a checkout.
mole_add_test(tst_RepositoryBand    SOURCES app/tst_RepositoryBand.cpp  LIBS mole_qml_harness mole_builtin)

# The session log lives in the application target rather than a library, so the
# test compiles the one file it is about. Exported symbols, so the backtrace it
# checks for carries names.
#
# Unix only, and it is the suite rather than the feature: the log is written on
# every platform, and what this asks is what survives a *crash* -- so every case
# in it crashes a forked child and reads what the child managed to write. fork(),
# waitpid() and a raised signal have no Windows equivalent worth writing, and a
# suite that cannot be built is what stops a whole tier rather than one case. See
# MOLE-124.
if(UNIX)
    mole_add_test(tst_SessionLog        SOURCES app/tst_SessionLog.cpp ../src/app/SessionLog.cpp)
    set_target_properties(tst_SessionLog PROPERTIES ENABLE_EXPORTS ON)
else()
    message(STATUS "Session log tests skipped: they crash a forked child, which is Unix's")
endif()

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

    # Which files a scan goes inside, asked of the real function with the real
    # factory behind it. Here rather than beside the other sdk code because it
    # needs the archive backend, which is what makes the answer real. See
    # MOLE-305.
    mole_add_test(tst_ScanReaders       SOURCES sdk/tst_ScanReaders.cpp
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

    # Only where Samba's client library was found, like the backend itself.
    if(SMBCLIENT_FOUND)
        mole_add_test(tst_SmbFileSystem SOURCES plugins/tst_SmbFileSystem.cpp
                                        LIBS mole_network_backends CURL::libcurl)
    endif()

    # Likewise for libnfs.
    if(LIBNFS_FOUND)
        mole_add_test(tst_NfsFileSystem SOURCES plugins/tst_NfsFileSystem.cpp
                                        LIBS mole_network_backends CURL::libcurl)
    endif()

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

    # And one case out of that same binary a second time, without the `heavy` label,
    # so it runs in `make test` where nothing else in that file can. How much of the
    # payload each destination gets is arithmetic, it needs no server and no minutes,
    # and it is the arithmetic that made `make release` refuse itself on two skips --
    # a fault found against a real machine should leave behind a test that does not
    # need one. See MOLE-320.
    add_test(NAME tst_HeavyPayloadSizing
             COMMAND tst_HeavyTransfers thePayloadIsSizedToTheDestinationItGoesTo)
    set_tests_properties(tst_HeavyPayloadSizing PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    # The same transfers with the server being attacked while they run. Needs
    # the control channel as well as an account, and skips itself with the
    # reason when either is missing.
    mole_add_test(tst_Interference     SOURCES scale/tst_Interference.cpp scale/HeavyPayload.cpp
                                       LIBS mole_network_backends CURL::libcurl)
    set_tests_properties(tst_Interference PROPERTIES TIMEOUT 3600 LABELS heavy)
else()
    message(STATUS "Network drive tests skipped: the network plugin was not built")
endif()

endif() # MOLE_CORE_ONLY
