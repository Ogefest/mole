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

    /// Whether a write to this backend goes under a working name and is put in
    /// place at the end (ADR-0020), which is what lets it tell an overwrite from
    /// a file that arrived while the write was running.
    ///
    /// True by default, because that is the contract and a new backend should
    /// have to say why not: two of six drives were exempt from it for months
    /// without anybody having decided so (MOLE-346). Set false only for a
    /// backend that puts bytes at the destination as it goes -- a bucket PUT
    /// lands on the key, and the in-memory drive writes straight into its map --
    /// which has no moment at which it could look. See TODO.md.
    bool stagesWrites = true;

    /// Runs `check` with the directory at `relativePath` made unlistable by this
    /// account, and puts it back afterwards. False means it could not be done
    /// and the case is skipped -- root reads a directory with no permissions at
    /// all, and a server may not let a test change a mode.
    ///
    /// Setting up and undoing it belongs to whoever built the context, because
    /// only they know what this drive is: a chmod, an injected fault, or nothing
    /// available at all. Leaving a directory nobody can read behind would take
    /// the temporary tree with it.
    std::function<bool(const QString& relativePath, const std::function<void()>& check)> whileUnlistable;
};

/// Runs the shared suite. Failures are reported through QTest, so call this
/// from inside a test slot.
void runFileSystemConformance(const ConformanceContext& context);

} // namespace mole::test
