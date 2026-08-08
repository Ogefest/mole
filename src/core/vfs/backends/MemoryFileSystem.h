#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QHash>
#include <QMutex>

namespace mole {

/// A complete filesystem living in RAM.
///
/// It exists for two reasons. It is a real scratch drive the user can mount,
/// and it is the backend the test suite runs almost everything against --
/// deterministic, fast, and able to fake the failures that are impossible to
/// reproduce on demand with real hardware (see setFault()).
class MemoryFileSystem final : public IFileSystem, public std::enable_shared_from_this<MemoryFileSystem>
{
public:
    MemoryFileSystem();

    QString scheme() const override { return QStringLiteral("mem"); }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target) override;

    // ---- test / fixture helpers -----------------------------------------

    /// Creates a file and every missing parent directory.
    void addFile(const QString& path, const QByteArray& contents = {}, const QDateTime& modified = {});
    void addDirectory(const QString& path);

    /// Makes every operation touching `path` fail with `error`. Pass
    /// VfsError::None to clear. This is how the tests cover "the NAS went away
    /// half way through a scan" without needing a NAS.
    void setFault(const QString& path, VfsError::Code error);
    void clearFaults();

    /// Sleeps this long inside list() -- used to test cancellation and to keep
    /// the UI honest about slow backends.
    void setListDelayMs(int ms) { m_listDelayMs = ms; }
    /// The same for openRead(), because the honest way to test what a view does
    /// while a file is slow to arrive is to have one that is.
    void setReadDelayMs(int ms) { m_readDelayMs = ms; }

    int listCallCount() const;

private:
    struct Node
    {
        bool isDir = false;
        QByteArray contents;
        QDateTime modified;
    };

    static QString normalise(const QString& path);
    VfsUri uriFor(const QString& path) const;
    Result<void> faultFor(const QString& path) const;

    mutable QMutex m_mutex;
    QHash<QString, Node> m_nodes;
    QHash<QString, VfsError::Code> m_faults;
    int m_listDelayMs = 0;
    int m_readDelayMs = 0;
    mutable int m_listCalls = 0;
};

class MemoryFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("mem"); }
    QString displayName() const override { return QStringLiteral("In-memory scratch"); }
    QString iconName() const override { return QStringLiteral("drive-removable-media"); }

    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;
};

} // namespace mole
