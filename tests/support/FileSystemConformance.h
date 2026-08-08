#pragma once

#include "core/vfs/IFileSystem.h"

#include <functional>

namespace mole::test {

/// Everything a backend has to get right, written once.
///
/// This is the contract behind "operations are identical on every drive".
/// When SFTP, S3 or WebDAV arrives, its test file is a handful of lines that
/// builds a context and calls runFileSystemConformance() -- if the new backend
/// disagrees with the local disk about what NotFound means or whether listing
/// a file is an error, this catches it before any UI code sees it.
struct ConformanceContext
{
    /// The backend under test.
    FileSystemPtr fileSystem;

    /// An existing, empty directory the suite may freely write into.
    VfsUri root;

    /// Creates a file out-of-band (not through the backend), so read-only
    /// backends can be tested too. Path is relative to `root`.
    std::function<bool(const QString& relativePath, const QByteArray& contents)> seedFile;
    /// Creates a directory out-of-band. Path is relative to `root`.
    std::function<bool(const QString& relativePath)> seedDir;

    /// Set to false for read-only backends; the mutating sections are skipped.
    bool expectsWriteSupport = true;
};

/// Runs the shared suite. Failures are reported through QTest, so call this
/// from inside a test slot.
void runFileSystemConformance(const ConformanceContext& context);

} // namespace mole::test
