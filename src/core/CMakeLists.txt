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
    vfs/IFileSystem.cpp
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
    tasks/FolderSizesTask.h
    tasks/FolderSizesTask.cpp

    index/IndexDatabase.h
    index/IndexDatabase.cpp
    index/ScanTask.h
    index/ScanTask.cpp
    index/IndexSearchTask.h
    index/IndexSearchTask.cpp

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
    PUBLIC Qt6::Core Qt6::Sql
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
