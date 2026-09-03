#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QHash>
#include <QMutex>

#include <atomic>

namespace mole::test {

/// A drive that contributes actions of its own, and can read the earlier states
/// of a file it hands out.
///
/// Nothing else in the tree offers a drive-contributed action, so everything
/// built on that tier could otherwise only be tested against a real filesystem
/// or a real bucket -- which means, in practice, not tested. What is in doubt is
/// never ZFS or S3: it is whether a row shows a mark, whether a picker offers
/// what the drive listed, and whether choosing one opens the state it names.
///
/// It offers **both outcome kinds**, because there are only two and both need a
/// test that needs no network:
///
/// - `linkAction()` answers with text, the way a container signing a link does.
/// - `versionsAction()` answers with other uris for the same file. They are
///   real: the drive understands versions and reads each one back with contents
///   of its own, so a test can assert *which* was opened rather than only that
///   something was.
///
/// A memory drive underneath, so it passes the conformance suite like any other
/// backend -- see tst_OfferingFileSystem.
class OfferingFileSystem final : public IFileSystem
{
public:
    /// Answers with text: a link that stops working after a while.
    static QString linkAction() { return QStringLiteral("org.mole.test.link"); }
    /// Answers with other uris for the same file.
    static QString versionsAction() { return QStringLiteral("org.mole.test.versions"); }

    explicit OfferingFileSystem(std::shared_ptr<MemoryFileSystem> inner = {});

    /// The drive underneath, for seeding files the ordinary way.
    const std::shared_ptr<MemoryFileSystem>& memory() const { return m_inner; }

    // ---- the fixture ------------------------------------------------------

    /// Seeds an earlier state of `path`, readable at the uri the versions action
    /// hands out. Contents that differ per version are the point: a test asserts
    /// which one was opened, not that something opened.
    void addVersion(const QString& path, const QString& token, const QByteArray& contents);

    /// Whether a link can be made for `path`. Settable per path because the case
    /// the row markers have to get right is a folder where some entries have
    /// something to offer and some do not.
    void setLinkable(const QString& path, bool linkable);

    /// Makes performing one fail the way a far end that has gone away does. What
    /// the drive offers is unchanged: an action can be on offer and still not
    /// work, which is the case somebody has to be told about.
    void setActionFault(VfsError::Code error) { m_actionFault = error; }

    /// Makes asking the drive slow, and cancellable where there is a token to
    /// poll. The query that feeds the markers runs on a worker thread and has to
    /// honour its CancelToken like every other call into storage.
    void setActionDelayMs(int ms) { m_actionDelayMs = ms; }
    /// Whether the drive is inside an action right now. What a test waits for,
    /// rather than waiting on a clock.
    bool isWorking() const { return m_working.load(); }

    /// How many times each was really asked.
    int actionsForCallCount() const;
    int invokeCallCount() const;
    /// How many times the whole folder was asked about. The claim this fake
    /// exists to let a test make is "one query per directory, whatever the
    /// number of entries", and only counting proves it.
    int folderQueryCallCount() const;

    // ---- IFileSystem ------------------------------------------------------

    QString scheme() const override;
    VfsCapabilities capabilities() const override;
    Qt::CaseSensitivity pathCaseSensitivity() const override;
    NameRules nameRules() const override;
    /// True, and it means it: openRead() and stat() answer about the state the
    /// uri names rather than about the file as it is.
    bool understandsVersions() const override { return true; }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<void> makeDirectory(const VfsUri& target) override;
    Result<QString> readLink(const VfsUri& link) override;
    Result<void> makeLink(const VfsUri& link, const QString& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;
    Result<void> replace(const VfsUri& from, const VfsUri& to) override;
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;

    FileActionList actionsFor(const VfsUri& target, const FileEntry& entry) override;
    Result<QStringList> entriesWithActions(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel) override;

protected:
    Result<QStringList> askWhatIsOffered(const VfsUri& target, const CancelToken& cancel) override;

private:
    /// The tokens seeded for a path, in the order they were added.
    QStringList versionsOf(const QString& path) const;
    /// Sleeps the configured delay in chunks, answering whether it ran to the
    /// end rather than being called off.
    bool waitOut(const CancelToken& cancel);

    std::shared_ptr<MemoryFileSystem> m_inner;

    mutable QMutex m_mutex;
    /// path -> (token -> contents), and the order tokens were added in.
    QHash<QString, QList<QPair<QString, QByteArray>>> m_versions;
    QHash<QString, bool> m_linkable;
    VfsError::Code m_actionFault = VfsError::None;
    int m_actionDelayMs = 0;
    mutable int m_actionsForCalls = 0;
    mutable int m_invokeCalls = 0;
    mutable int m_folderQueries = 0;
    std::atomic_bool m_working { false };
};

} // namespace mole::test
