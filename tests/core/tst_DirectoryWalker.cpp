#include "support/MoleTestMain.h"

#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;

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
