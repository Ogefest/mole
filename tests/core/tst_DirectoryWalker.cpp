#include "support/MoleTestMain.h"

#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <functional>

using namespace mole;

namespace {

/// A drive that changes while it is being walked.
///
/// The first listing of a chosen directory runs the mutation and then answers,
/// so the walker is handed a directory that is already out of date -- a file
/// that has gone, a directory that is now a file, a name that was renamed while
/// the walker was looking away. That is not a contrived arrangement: it is a
/// build running in the folder somebody is copying.
class ChurningFileSystem final : public IFileSystem
{
public:
    ChurningFileSystem(FileSystemPtr inner, QString path, std::function<void()> churn)
        : m_inner(std::move(inner))
        , m_path(std::move(path))
        , m_churn(std::move(churn))
    {
    }

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override
    {
        // The answer is taken first and the tree changed afterwards, which is
        // the arrangement that matters: what the walker holds is a listing that
        // was true a moment ago and is not true now.
        Result<FileEntryList> listed = m_inner->list(dir, cancel);
        if (dir.path() == m_path && !m_churned) {
            m_churned = true;
            m_churn();
        }
        return listed;
    }
    Result<FileEntry> stat(const VfsUri& target) override { return m_inner->stat(target); }
    Result<void> makeDirectory(const VfsUri& target) override { return m_inner->makeDirectory(target); }
    Result<void> remove(const VfsUri& target, bool recursive, const CancelToken&) override
    {
        return m_inner->remove(target, recursive);
    }
    Result<void> rename(const VfsUri& from, const VfsUri& to, const CancelToken&) override
    {
        return m_inner->rename(from, to);
    }
    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& = {}) override
    {
        return m_inner->openRead(target, expectedSize);
    }
    Result<std::unique_ptr<QIODevice>> openWrite(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& = {}) override
    {
        return m_inner->openWrite(target, expectedSize);
    }

private:
    FileSystemPtr m_inner;
    QString m_path;
    std::function<void()> m_churn;
    bool m_churned = false;
};

} // namespace

class TestDirectoryWalker : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void visitsEveryEntryRecursively();
    void respectsMaxDepth();
    void visitorCanPruneASubtree();
    void stopEndsTheWalkSuccessfully();
    void unreadableDirectoryIsRecordedNotFatal();
    void cancellationStopsTheWalk();
    void skipsHiddenWhenAsked();
    void emptyRootYieldsNothing();
    void aShortcutIsWalkedAndASymbolicLinkIsNot();
    void aTreeThatChangesUnderTheWalkIsReportedAndTheWalkFinishes();

private:
    std::shared_ptr<MemoryFileSystem> m_fs;
    VfsUri m_root;
};

void TestDirectoryWalker::init()
{
    m_fs = std::make_shared<MemoryFileSystem>();
    m_root = VfsUri::fromString(QStringLiteral("mem:///"));
}

void TestDirectoryWalker::visitsEveryEntryRecursively()
{
    m_fs->addFile(QStringLiteral("/a.txt"));
    m_fs->addFile(QStringLiteral("/one/b.txt"));
    m_fs->addFile(QStringLiteral("/one/two/c.txt"));

    QStringList seen;
    DirectoryWalker walker(m_fs);
    Result<void> result = walker.walk(m_root, CancelToken(), [&](const FileEntry& entry, int) {
        seen.append(entry.uri.path());
        return DirectoryWalker::Action::Continue;
    });

    QVERIFY2(result.ok(), qPrintable(result.error().message));
    seen.sort();
    const QStringList expected { QStringLiteral("/a.txt"), QStringLiteral("/one"),
        QStringLiteral("/one/b.txt"), QStringLiteral("/one/two"), QStringLiteral("/one/two/c.txt") };
    QCOMPARE(seen, expected);
    QCOMPARE(walker.visitedCount(), 5);
}

void TestDirectoryWalker::respectsMaxDepth()
{
    m_fs->addFile(QStringLiteral("/a.txt"));
    m_fs->addFile(QStringLiteral("/one/b.txt"));
    m_fs->addFile(QStringLiteral("/one/two/c.txt"));

    DirectoryWalker::Options options;
    options.maxDepth = 0; // direct children of the root only

    QStringList seen;
    DirectoryWalker walker(m_fs, options);
    QVERIFY(walker
                .walk(m_root, CancelToken(),
                    [&](const FileEntry& entry, int) {
                        seen.append(entry.name);
                        return DirectoryWalker::Action::Continue;
                    })
                .ok());

    seen.sort();
    QCOMPARE(seen, QStringList({ QStringLiteral("a.txt"), QStringLiteral("one") }));
}

void TestDirectoryWalker::visitorCanPruneASubtree()
{
    m_fs->addFile(QStringLiteral("/keep/wanted.txt"));
    m_fs->addFile(QStringLiteral("/skip/unwanted.txt"));

    QStringList seen;
    DirectoryWalker walker(m_fs);
    QVERIFY(walker
                .walk(m_root, CancelToken(),
                    [&](const FileEntry& entry, int) {
                        seen.append(entry.name);
                        return entry.name == QLatin1String("skip") ? DirectoryWalker::Action::SkipSubtree
                                                                   : DirectoryWalker::Action::Continue;
                    })
                .ok());

    QVERIFY(seen.contains(QStringLiteral("wanted.txt")));
    QVERIFY2(!seen.contains(QStringLiteral("unwanted.txt")),
        "SkipSubtree must stop the descent into that directory");
    // The pruned directory itself is still reported.
    QVERIFY(seen.contains(QStringLiteral("skip")));
    QVERIFY2(!walker.stoppedEarly(), "pruning one subtree is not stopping the walk");
}

void TestDirectoryWalker::stopEndsTheWalkSuccessfully()
{
    for (int i = 0; i < 30; ++i)
        m_fs->addFile(QStringLiteral("/dir%1/file.txt").arg(i));

    int visited = 0;
    DirectoryWalker walker(m_fs);
    Result<void> result = walker.walk(m_root, CancelToken(), [&](const FileEntry&, int) {
        return ++visited == 3 ? DirectoryWalker::Action::Stop : DirectoryWalker::Action::Continue;
    });

    // Stopping is how a capped search ends. It is a success, not a failure and
    // not a cancellation -- the caller decides how to present the truncation.
    QVERIFY2(result.ok(), "Stop must end the walk successfully");
    QVERIFY(walker.stoppedEarly());
    QCOMPARE(visited, 3);
}

void TestDirectoryWalker::aTreeThatChangesUnderTheWalkIsReportedAndTheWalkFinishes()
{
    // Four things happen to one directory between the walker being told what is
    // in it and the walker going to look: a file appears, a file goes, a
    // directory is renamed out from under the descent, and a directory becomes a
    // file of the same name. None of them may abort the walk, and none of them
    // may send it round a second time.
    m_fs->addFile(QStringLiteral("/tree/steady.txt"));
    m_fs->addFile(QStringLiteral("/tree/vanishing.txt"));
    m_fs->addFile(QStringLiteral("/tree/renamed/inside.txt"));
    m_fs->addFile(QStringLiteral("/tree/becomes-a-file/inside.txt"));

    auto churning = std::make_shared<ChurningFileSystem>(m_fs, QStringLiteral("/tree"), [this] {
        m_fs->addFile(QStringLiteral("/tree/appeared.txt"));
        m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///tree/vanishing.txt")), false);
        m_fs->rename(VfsUri::fromString(QStringLiteral("mem:///tree/renamed")),
            VfsUri::fromString(QStringLiteral("mem:///tree/elsewhere")));
        m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///tree/becomes-a-file")), true);
        m_fs->addFile(QStringLiteral("/tree/becomes-a-file"), QByteArray("now a file"));
    });

    QStringList seen;
    DirectoryWalker walker(churning);
    Result<void> result = walker.walk(
        VfsUri::fromString(QStringLiteral("mem:///tree")), CancelToken(), [&](const FileEntry& entry, int) {
            seen.append(entry.name);
            return DirectoryWalker::Action::Continue;
        });

    QVERIFY2(result.ok(), qPrintable(result.error().message));
    // What the listing said is what it walked: a file that has since gone is
    // still reported, and one that turned up after the listing is not.
    QVERIFY(seen.contains(QStringLiteral("steady.txt")));
    QVERIFY(seen.contains(QStringLiteral("vanishing.txt")));
    QVERIFY2(!seen.contains(QStringLiteral("appeared.txt")), "the listing was taken before it appeared");
    // The renamed directory and the one that became a file were both listed as
    // directories and are neither of them there to descend into now. That is an
    // error about those two, and not a reason to stop.
    QVERIFY(seen.contains(QStringLiteral("renamed")));
    QVERIFY(seen.contains(QStringLiteral("becomes-a-file")));
    QVERIFY(!seen.contains(QStringLiteral("inside.txt")));
    QVERIFY2(!walker.errors().isEmpty(), "a subtree that went away under the walk has to be reported");
    // And nothing was visited twice, which is what a walk that follows a moving
    // tree round in a circle would do.
    QStringList sorted = seen;
    sorted.sort();
    for (int i = 1; i < sorted.size(); ++i)
        QVERIFY2(sorted.at(i) != sorted.at(i - 1),
            qPrintable(QStringLiteral("visited twice: %1").arg(sorted.at(i))));
}

void TestDirectoryWalker::unreadableDirectoryIsRecordedNotFatal()
{
    m_fs->addFile(QStringLiteral("/ok/file.txt"));
    m_fs->addFile(QStringLiteral("/locked/secret.txt"));
    m_fs->setFault(QStringLiteral("/locked"), VfsError::AccessDenied);

    QStringList seen;
    DirectoryWalker walker(m_fs);
    Result<void> result = walker.walk(m_root, CancelToken(), [&](const FileEntry& entry, int) {
        seen.append(entry.name);
        return DirectoryWalker::Action::Continue;
    });

    // One permission denied must not abort a scan of everything else.
    QVERIFY2(result.ok(), "an unreadable subdirectory must not fail the whole walk");
    QVERIFY(seen.contains(QStringLiteral("file.txt")));
    QVERIFY(!seen.contains(QStringLiteral("secret.txt")));
    QCOMPARE(walker.errors().size(), 1);
    QCOMPARE(walker.errors().first().code, VfsError::AccessDenied);
}

void TestDirectoryWalker::cancellationStopsTheWalk()
{
    for (int i = 0; i < 50; ++i)
        m_fs->addFile(QStringLiteral("/dir%1/file.txt").arg(i));

    CancelToken token;
    int visited = 0;

    DirectoryWalker walker(m_fs);
    Result<void> result = walker.walk(m_root, token, [&](const FileEntry&, int) {
        if (++visited == 5)
            token.cancel();
        return DirectoryWalker::Action::Continue;
    });

    QVERIFY(!result.ok());
    QCOMPARE(result.error().code, VfsError::Cancelled);
    QVERIFY2(visited < 50, "the walk must stop promptly after cancellation");
}

void TestDirectoryWalker::skipsHiddenWhenAsked()
{
    m_fs->addFile(QStringLiteral("/.hidden"));
    m_fs->addFile(QStringLiteral("/visible"));

    DirectoryWalker::Options options;
    options.includeHidden = false;

    QStringList seen;
    DirectoryWalker walker(m_fs, options);
    QVERIFY(walker
                .walk(m_root, CancelToken(),
                    [&](const FileEntry& entry, int) {
                        seen.append(entry.name);
                        return DirectoryWalker::Action::Continue;
                    })
                .ok());

    QCOMPARE(seen, QStringList({ QStringLiteral("visible") }));
}

void TestDirectoryWalker::aShortcutIsWalkedAndASymbolicLinkIsNot()
{
    // On Windows QFileInfo::isSymLink() is true for an NTFS symbolic link, a
    // junction, and a .lnk shortcut -- which is not a link at all but an
    // ordinary file that happens to contain a target. So a folder of shortcuts
    // was silently walked past by every sync plan and duplicate scan, which is
    // what this walker's default does with a link it was not asked to follow.
    //
    // The fault is Windows-only and the rule is not, so it is held against a
    // MemoryFileSystem carrying one of each and runs on any machine.
    m_fs->addFile(QStringLiteral("/plain/here.txt"));
    m_fs->addFile(QStringLiteral("/linked/there.txt"));
    m_fs->addFile(QStringLiteral("/shortcut/also.txt"));
    m_fs->markAsSymlink(QStringLiteral("/linked"));
    m_fs->markAsShortcut(QStringLiteral("/shortcut"));

    QStringList seen;
    DirectoryWalker walker(m_fs); // followSymlinks off, as a sync plan and a duplicate scan have it
    Result<void> result = walker.walk(m_root, CancelToken(), [&](const FileEntry& entry, int) {
        seen.append(entry.uri.path());
        return DirectoryWalker::Action::Continue;
    });
    QVERIFY2(result.ok(), qPrintable(result.error().message));
    seen.sort();

    // The link itself is reported and not descended into.
    QVERIFY(seen.contains(QStringLiteral("/linked")));
    QVERIFY2(!seen.contains(QStringLiteral("/linked/there.txt")), qPrintable(seen.join(u',')));

    // The shortcut is not a link, so what is under it is walked like anything
    // else. This is the assertion that fails today on the machine that has
    // shortcuts.
    QVERIFY(seen.contains(QStringLiteral("/shortcut")));
    QVERIFY2(seen.contains(QStringLiteral("/shortcut/also.txt")), qPrintable(seen.join(u',')));

    QVERIFY(seen.contains(QStringLiteral("/plain/here.txt")));

    // And the two are told apart in the listing as well as by the walk, because
    // marking a shortcut as a link in the interface is saying something untrue
    // about it.
    Result<FileEntryList> listed = m_fs->list(m_root, CancelToken());
    QVERIFY(listed.ok());
    for (const FileEntry& entry : listed.value()) {
        if (entry.name == QLatin1String("linked")) {
            QVERIFY(entry.isSymlink);
            QVERIFY(!entry.isShortcut);
        } else if (entry.name == QLatin1String("shortcut")) {
            QVERIFY(!entry.isSymlink);
            QVERIFY(entry.isShortcut);
        } else {
            QVERIFY(!entry.isSymlink);
            QVERIFY(!entry.isShortcut);
        }
    }
}

void TestDirectoryWalker::emptyRootYieldsNothing()
{
    int calls = 0;
    DirectoryWalker walker(m_fs);
    QVERIFY(walker
                .walk(m_root, CancelToken(),
                    [&](const FileEntry&, int) {
                        ++calls;
                        return DirectoryWalker::Action::Continue;
                    })
                .ok());

    QCOMPARE(calls, 0);
    QCOMPARE(walker.visitedCount(), 0);
    QVERIFY(walker.errors().isEmpty());
}

MOLE_TEST_MAIN(TestDirectoryWalker)
#include "tst_DirectoryWalker.moc"
