# mole_core holds every piece of logic worth testing: the VFS abstraction, the
# task scheduler, the event bus and the index. It links no GUI module, which is
# what lets the whole test suite run headless and fast.
add_library(mole_core STATIC
    CoreMetaTypes.h
    CoreMetaTypes.cpp

    vfs/VfsTypes.h
    vfs/VfsUri.h
    vfs/VfsUri.cpp
    vfs/FileEntry.h
    vfs/IFileSystem.h
    vfs/PartialWrite.h
    vfs/IFileSystem.cpp
    vfs/PartialWrite.cpp
    vfs/IFileSystemFactory.h
    vfs/VfsManager.h
    vfs/VfsManager.cpp
    vfs/DirectoryWalker.h
    vfs/DirectoryWalker.cpp
    vfs/RemoteRegistry.h
    vfs/RemoteRegistry.cpp
    vfs/SystemVolumes.h
    vfs/SystemVolumes.cpp
    vfs/backends/LocalFileSystem.h
    vfs/backends/LocalFileSystem.cpp
    vfs/backends/MemoryFileSystem.h
    vfs/backends/MemoryFileSystem.cpp

    diagnostics/Diagnostics.h
    diagnostics/Diagnostics.cpp
    diagnostics/LoggingFileSystem.h
    diagnostics/LoggingFileSystem.cpp

    events/EventBus.h
    events/EventBus.cpp

    alerts/AlertRule.h
    alerts/AlertRule.cpp
    alerts/AlertStore.h
    alerts/AlertStore.cpp
    alerts/AlertEvaluator.h
    alerts/AlertEvaluator.cpp
    alerts/CheckAlertsTask.h
    alerts/CheckAlertsTask.cpp

    automation/ScheduleRule.h
    automation/ScheduleRule.cpp
    automation/ScheduleStore.h
    automation/ScheduleStore.cpp
    automation/Scheduler.h
    automation/Scheduler.cpp

    tasks/Task.h
    tasks/Task.cpp
    tasks/TaskManager.h
    tasks/TaskManager.cpp
    tasks/ListDirectoryTask.h
    tasks/ListDirectoryTask.cpp
    tasks/ReadFileTask.h
    tasks/ReadFileTask.cpp
    tasks/QuerySpaceTask.h
    tasks/QuerySpaceTask.cpp
    tasks/QueryAccessTask.h
    tasks/QueryAccessTask.cpp
    tasks/ReadRangeTask.h
    tasks/ReadRangeTask.cpp
    tasks/TransferTask.h
    tasks/TransferTask.cpp
    settings/Preferences.h
    settings/Preferences.cpp
    tasks/FolderSizesTask.h
    tasks/FolderSizesTask.cpp
    tasks/DriveCheckTask.h
    tasks/DriveCheckTask.cpp

    index/IndexDatabase.h
    index/IndexDatabase.cpp
    index/ScanTask.h
    index/ScanTask.cpp
    index/IndexSearchTask.h
    index/IndexSearchTask.cpp

    search/SearchQuery.h
    search/SearchQuery.cpp
    search/QueryLine.h
    search/QueryLine.cpp
    search/LiveSearchTask.h
    search/LiveSearchTask.cpp

    credentials/SecretStore.h
    credentials/SecretStore.cpp

    terminal/Pty.h
    terminal/Pty.cpp
    terminal/TerminalScreen.h
    terminal/TerminalScreen.cpp

    sync/SyncOptions.h
    sync/SyncOptions.cpp
    sync/SyncPlan.h
    sync/SyncPlan.cpp
    sync/SyncTask.h
    sync/SyncTask.cpp

    duplicates/DuplicateStrategy.h
    duplicates/Strategies.h
    duplicates/Strategies.cpp
    duplicates/FindDuplicatesTask.h
    duplicates/FindDuplicatesTask.cpp

    rename/RenameRule.h
    rename/RenameRule.cpp
    rename/RenamePlan.h
    rename/RenamePlan.cpp
    rename/RenameTask.h
    rename/RenameTask.cpp

    sets/FileSet.h
    sets/FileSet.cpp
    sets/FileSetStore.h
    sets/FileSetStore.cpp
    sets/VerifySetTask.h
    sets/VerifySetTask.cpp

    data/FileType.h
    data/FileType.cpp
    data/ITableSource.h
    data/SqliteTable.h
    data/SqliteTable.cpp
    data/ParquetTable.h
    data/ParquetTable.cpp

    text/DelimitedText.h
    text/DelimitedText.cpp
    text/DelimitedStreamParser.h
    text/DelimitedStreamParser.cpp
    text/DelimitedStore.h
    text/DelimitedStore.cpp
    text/ImportDelimitedTask.h
    text/ImportDelimitedTask.cpp
    text/ImportDelimitedTask.h
    text/ImportDelimitedTask.cpp

    vcs/Repository.h
    vcs/Repository.cpp
    vcs/ReadRepositoryTask.h
    vcs/ReadRepositoryTask.cpp

    analysis/AnalysisReport.h
    analysis/AnalysisReport.cpp
    analysis/AnalyseDirectoryTask.h
    analysis/AnalyseDirectoryTask.cpp
    analysis/AnalysisStore.h
    analysis/AnalysisStore.cpp
    analysis/ReportDiff.h
    analysis/ReportDiff.cpp
)

add_library(mole::core ALIAS mole_core)

# Headers are spelled "core/..." from everywhere, including inside core itself,
# so any given header has exactly one include path.
target_include_directories(mole_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(mole_core
    # Concurrent for the one thing that is worth overlapping: a content search
    # spends its time waiting for storage, so the candidate files are opened
    # several at a time.
    PUBLIC Qt6::Core Qt6::Sql Qt6::Concurrent
    PRIVATE mole_flags)

# forkpty lives in libutil on Linux. The terminal panel is the only thing that
# needs it, and on a platform without it Pty reports itself unavailable instead.
if(UNIX AND NOT APPLE)
    target_link_libraries(mole_core PRIVATE util)
endif()

# Parquet is optional. When Arrow is installed the viewer reads Parquet files in
# place; when it is not, ParquetTable reports itself unsupported and the preview
# layer falls through to the file-information viewer. A missing optional library
# must never stop the application being built.
find_package(Arrow QUIET)
find_package(Parquet QUIET)
if(Arrow_FOUND AND Parquet_FOUND)
    target_compile_definitions(mole_core PUBLIC MOLE_HAVE_PARQUET)
    target_link_libraries(mole_core PRIVATE Arrow::arrow_shared Parquet::parquet_shared)
    message(STATUS "Parquet preview: enabled (Arrow ${ARROW_VERSION})")
else()
    message(STATUS "Parquet preview: disabled (install libarrow-dev and libparquet-dev)")
endif()

# A complete terminal emulator, when one is available. Without it the built-in
# parser handles line-oriented output and says so when it meets something it
# cannot draw -- the panel works either way, but only libvterm makes a
# full-screen program behave.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VTERM QUIET vterm)
endif()
if(VTERM_FOUND)
    target_compile_definitions(mole_core PUBLIC MOLE_HAVE_VTERM)
    target_include_directories(mole_core PRIVATE ${VTERM_INCLUDE_DIRS})
    target_link_libraries(mole_core PRIVATE ${VTERM_LIBRARIES})
    message(STATUS "Terminal: libvterm ${VTERM_VERSION}")
else()
    message(STATUS "Terminal: built-in parser (install libvterm-dev for full support)")
endif()

# Git state, when libgit2 is available. Without it a folder inside a checkout
# looks exactly like one that is not -- no band, no markers, and nothing that
# fails: Repository::isSupported() answers no and every caller shows nothing.
# See docs/adr/0041-git-state-is-read-through-libgit2.md.
#
# MOLE_WITH_GIT2 exists so a machine that has the library can still build the way
# a machine without it does. That build is the one nobody would otherwise try
# until a contributor hit it.
option(MOLE_WITH_GIT2 "Read git state through libgit2 when it is available" ON)
if(PkgConfig_FOUND AND MOLE_WITH_GIT2)
    pkg_check_modules(GIT2 QUIET libgit2)
endif()
if(GIT2_FOUND)
    target_compile_definitions(mole_core PUBLIC MOLE_HAVE_GIT2)
    target_include_directories(mole_core PRIVATE ${GIT2_INCLUDE_DIRS})
    target_link_libraries(mole_core PRIVATE ${GIT2_LIBRARIES})
    message(STATUS "Git state: libgit2 ${GIT2_VERSION}")
    # The tests build their fixture repositories through libgit2 themselves, so
    # they need what core links rather than only what it defines. Cached, because
    # the test tree is configured from a different directory scope.
    set(MOLE_GIT2_FOUND ON CACHE INTERNAL "")
    set(MOLE_GIT2_INCLUDE_DIRS "${GIT2_INCLUDE_DIRS}" CACHE INTERNAL "")
    set(MOLE_GIT2_LIBRARIES "${GIT2_LIBRARIES}" CACHE INTERNAL "")
    set(MOLE_GIT2_LIBRARY_DIRS "${GIT2_LIBRARY_DIRS}" CACHE INTERNAL "")
else()
    if(MOLE_WITH_GIT2)
        message(STATUS "Git state: disabled (install libgit2-dev)")
    else()
        message(STATUS "Git state: disabled (MOLE_WITH_GIT2=OFF)")
    endif()
    set(MOLE_GIT2_FOUND OFF CACHE INTERNAL "")
endif()

# Credentials are encrypted with OpenSSL, which Qt already depends on. Without it
# the store refuses to hold anything rather than falling back to writing secrets
# in the clear -- a credential store that quietly stops encrypting is worse than
# one that says it cannot.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_compile_definitions(mole_core PUBLIC MOLE_HAVE_OPENSSL)
    target_link_libraries(mole_core PRIVATE OpenSSL::Crypto)
    message(STATUS "Credential store: OpenSSL ${OPENSSL_VERSION}")
else()
    message(STATUS "Credential store: unavailable (install libssl-dev)")
endif()
