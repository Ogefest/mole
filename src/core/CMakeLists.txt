# mole_core holds every piece of logic worth testing: the VFS abstraction, the
# task scheduler, the event bus and the index. It links no GUI module, which is
# what lets the whole test suite run headless and fast.
add_library(mole_core STATIC
    CoreMetaTypes.h
    CoreMetaTypes.cpp

    platform/HostPlatform.h
    platform/Staging.h
    platform/Staging.cpp

    vfs/VfsTypes.h
    vfs/VfsUri.h
    vfs/VfsUri.cpp
    vfs/FileEntry.h
    vfs/FileAction.h
    vfs/IFileSystem.h
    vfs/PartialWrite.h
    vfs/IFileSystem.cpp
    vfs/PartialWrite.cpp
    vfs/IFileSystemFactory.h
    vfs/VersionGuard.h
    vfs/VersionGuard.cpp
    vfs/VfsManager.h
    vfs/VfsManager.cpp
    vfs/DirectoryWalker.h
    vfs/DirectoryWalker.cpp
    vfs/RemoteRegistry.h
    vfs/RemoteRegistry.cpp
    vfs/NameRules.h
    vfs/NameRules.cpp
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
    automation/Chain.h
    automation/Chain.cpp
    automation/ChainSources.h
    automation/ChainSources.cpp
    automation/ChainSteps.h
    automation/ChainSteps.cpp
    automation/ChainStore.h
    automation/ChainStore.cpp
    automation/ChainTask.h
    automation/ChainTask.cpp
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
    tasks/MakeDirectoryTask.h
    tasks/MakeDirectoryTask.cpp
    tasks/StatTask.h
    tasks/StatTask.cpp
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
    tasks/ProbeDriveTask.h
    tasks/ProbeDriveTask.cpp
    tasks/QueryFileActionsTask.h
    tasks/QueryFileActionsTask.cpp
    tasks/QueryFolderActionsTask.h
    tasks/QueryFolderActionsTask.cpp
    tasks/InvokeFileActionTask.h
    tasks/InvokeFileActionTask.cpp
    tasks/SweepLeftoversTask.h
    tasks/SweepLeftoversTask.cpp

    index/IndexDatabase.h
    index/IndexDatabase.cpp
    index/ScanOptions.h
    index/ScanTask.h
    index/ScanTask.cpp
    index/IndexSearchTask.h
    index/IndexSearchTask.cpp
    index/IndexSummary.h
    index/IndexSummary.cpp
    index/ForgetVolumeTask.h
    index/ForgetVolumeTask.cpp

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
    duplicates/ContentComparison.h
    duplicates/ContentComparison.cpp

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
    data/JsonFileStore.h
    data/JsonFileStore.cpp
    data/SqliteConnection.h
    data/SqliteConnection.cpp
    data/SqliteError.h
    data/SqliteError.cpp
    data/ITableSource.h
    data/SqliteTable.h
    data/SqliteTable.cpp
    data/CountTableRowsTask.h
    data/CountTableRowsTask.cpp
    data/ParquetTable.h
    data/ParquetTable.cpp
    data/ReadTableTask.h
    data/ReadTableTask.cpp

    text/DelimitedText.h
    text/DelimitedText.cpp
    text/DelimitedStreamParser.h
    text/DelimitedStreamParser.cpp
    text/DelimitedStore.h
    text/DelimitedStore.cpp
    text/ImportDelimitedTask.h
    text/ImportDelimitedTask.cpp
    text/ImportJsonLinesTask.h
    text/ImportJsonLinesTask.cpp

    vcs/Repository.h
    vcs/Repository.cpp
    vcs/ReadRepositoryTask.h
    vcs/ReadRepositoryTask.cpp
    vcs/ReadStatusTask.h
    vcs/ReadStatusTask.cpp

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
#
# Every row below goes through mole_optional_dependency(), which is where that
# rule and the shape of the line it prints now live -- see
# cmake/MoleOptionalDependency.cmake and MOLE-390.
mole_optional_dependency(PARQUET
    SUMMARY "Parquet preview"
    PACKAGE Arrow Parquet
    VERSION_FROM ARROW_VERSION
    FOUND "enabled (Arrow @VERSION@)"
    MISSING "disabled (install libarrow-dev and libparquet-dev)"
    TARGET mole_core
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_PARQUET
    LINK Arrow::arrow_shared Parquet::parquet_shared)

# A complete terminal emulator, when one is available. Without it the built-in
# parser handles line-oriented output and says so when it meets something it
# cannot draw -- the panel works either way, but only libvterm makes a
# full-screen program behave.
mole_optional_dependency(VTERM
    SUMMARY "Terminal"
    PKG_CONFIG vterm
    FOUND "libvterm @VERSION@"
    MISSING "built-in parser (install libvterm-dev for full support)"
    TARGET mole_core
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_VTERM)

# Git state, when libgit2 is available. Without it a folder inside a checkout
# looks exactly like one that is not -- no band, no markers, and nothing that
# fails: Repository::isSupported() answers no and every caller shows nothing.
# See docs/adr/0041-git-state-is-read-through-libgit2.md.
#
# MOLE_WITH_GIT2 exists so a machine that has the library can still build the way
# a machine without it does. That build is the one nobody would otherwise try
# until a contributor hit it.
option(MOLE_WITH_GIT2 "Read git state through libgit2 when it is available" ON)
# The tests build their fixture repositories through libgit2 themselves, so they
# need what core links rather than only what it defines -- CACHE_PREFIX is how
# that crosses into the test tree, which is a directory scope of its own.
mole_optional_dependency(GIT2
    SUMMARY "Git state"
    PKG_CONFIG libgit2
    SWITCH MOLE_WITH_GIT2
    FOUND "libgit2 @VERSION@"
    MISSING "disabled (install libgit2-dev)"
    TARGET mole_core
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_GIT2
    CACHE_PREFIX MOLE_GIT2)

# The first stage of a content scan digests the head of each candidate. It is a
# filter and proves nothing -- the stage after it compares the files themselves --
# so it wants the fastest hash there is rather than a cryptographic one. XXH3 is
# roughly a hundred times faster than the SHA-256 this used to use, which is the
# difference between waiting for the processor and waiting for the disk. Without
# it the scan falls back to SHA-256: slower, and every bit as correct, because a
# filter only has to be consistent. See ADR-0046.
# PUBLIC like every other row, and it was the one PRIVATE define. Nothing outside
# core reads MOLE_HAVE_XXHASH today; a define that is PRIVATE for no reason is a
# difference the next reader has to account for, and the suite that checks the
# hash a scan used would need it the moment it asked.
mole_optional_dependency(XXHASH
    SUMMARY "Duplicate scan head"
    PKG_CONFIG libxxhash
    FOUND "xxhash @VERSION@"
    MISSING "SHA-256 (install libxxhash-dev for a faster filter)"
    TARGET mole_core
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_XXHASH)

# Credentials are encrypted with OpenSSL, which Qt already depends on. Without it
# the store refuses to hold anything rather than falling back to writing secrets
# in the clear -- a credential store that quietly stops encrypting is worse than
# one that says it cannot.
mole_optional_dependency(OPENSSL
    SUMMARY "Credential store"
    PACKAGE OpenSSL
    VERSION_FROM OPENSSL_VERSION
    FOUND "OpenSSL @VERSION@"
    MISSING "unavailable (install libssl-dev)"
    TARGET mole_core
    SCOPE PUBLIC
    DEFINE MOLE_HAVE_OPENSSL
    LINK OpenSSL::Crypto)
