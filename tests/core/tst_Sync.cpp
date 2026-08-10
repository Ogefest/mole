#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/sync/SyncTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;
using namespace mole::test;

/// Syncing one tree onto another: what each mode does, what it refuses to do,
/// and the dry run that has to be worth believing.
class TestSync : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void copiesWhatIsMissing();
    void updateNeverDeletes();
    void mirrorRemovesWhatTheSourceLacks();
    void fillGapsLeavesExistingFilesAlone();

    void skipsAFileThatIsNewerAtTheDestination();
    void overwritesWhenAskedTo();
    void comparesBySizeAlone();
    void comparesByContents();
    void toleratesSubSecondTimestampDrift();

    void filtersByPattern();
    void anIncludeBeatsABroadExclude();
    void aFilteredNameIsNeverDeletedByAMirror();

    void aSourceThatCannotBeListedDeletesNothing();
    void oneUnreadableDirectoryDeletesNothingInsideIt();
    void aSecondRunOverTheSameTreesDoesNothingAtAll();
    void sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe_data();
    void sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe();
    void aRenameIsNeverADeleteBeforeItIsACopy();

    void aDryRunWritesNothing();
    void aDryRunAgainstADriveThatRefusesEveryWriteStillReportsNoFailures();
    void directoriesArePlannedBeforeTheFilesInThem();
    void deletionsComeLast();

private:
    SyncTask* run(SyncOptions options);
    bool exists(const QString& relative) const;
    /// QFile::setFileTime needs the file open, which is easy to forget and
    /// silently returns false rather than complaining.
    bool touch(const QString& relative, const QDateTime& when) const;
    QByteArray contentsOf(const QString& relative) const;

    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<TaskManager> m_tasks;
    FileSystemPtr m_fs;
};

void TestSync::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->makeDirs(QStringLiteral("dest")));
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<LocalFileSystem>();
}

void TestSync::cleanup()
{
    m_tasks.reset();
    m_fs.reset();
    m_tree.reset();
}

SyncTask* TestSync::run(SyncOptions options)
{
    auto* task = new SyncTask(m_fs, m_tree->rootUri().child(QStringLiteral("src")), m_fs,
        m_tree->rootUri().child(QStringLiteral("dest")), std::move(options));
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 30000))
        return nullptr;
    return task;
}

bool TestSync::exists(const QString& relative) const
{
    return QFile::exists(QDir(m_tree->path()).filePath(relative));
}

bool TestSync::touch(const QString& relative, const QDateTime& when) const
{
    QFile file(QDir(m_tree->path()).filePath(relative));
    if (!file.open(QIODevice::ReadWrite))
        return false;
    const bool ok = file.setFileTime(when, QFileDevice::FileModificationTime);
    file.close();
    return ok;
}

QByteArray TestSync::contentsOf(const QString& relative) const
{
    QFile file(QDir(m_tree->path()).filePath(relative));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// ------------------------------------------------------------------ modes

void TestSync::copiesWhatIsMissing()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/deep/b.txt"), QByteArray("two")));

    SyncOptions options;
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(task->failures(), QStringList());
    QVERIFY(exists(QStringLiteral("dest/a.txt")));
    QVERIFY(exists(QStringLiteral("dest/deep/b.txt")));
    QCOMPARE(contentsOf(QStringLiteral("dest/deep/b.txt")), QByteArray("two"));
}

void TestSync::updateNeverDeletes()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/theirs.txt"), QByteArray("keep me")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Update;
    options.dryRun = false;

    QVERIFY(run(options));
    // The safe default, and what most people mean by "sync": nothing at the
    // destination is ever removed.
    QVERIFY(exists(QStringLiteral("dest/theirs.txt")));
    QVERIFY(exists(QStringLiteral("dest/a.txt")));
}

void TestSync::mirrorRemovesWhatTheSourceLacks()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/stale.txt"), QByteArray("old")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = false;

    QVERIFY(run(options));
    QVERIFY(exists(QStringLiteral("dest/a.txt")));
    QVERIFY2(!exists(QStringLiteral("dest/stale.txt")), "a mirror makes the two match exactly");
}

void TestSync::fillGapsLeavesExistingFilesAlone()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("new version")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), QByteArray("old")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/b.txt"), QByteArray("missing")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::FillGaps;
    options.dryRun = false;

    QVERIFY(run(options));
    QCOMPARE(contentsOf(QStringLiteral("dest/a.txt")), QByteArray("old"));
    QVERIFY(exists(QStringLiteral("dest/b.txt")));
}

// ------------------------------------------------------------- comparison

void TestSync::skipsAFileThatIsNewerAtTheDestination()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("from the source")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), QByteArray("done at the far end")));

    // Make the destination unambiguously newer.
    QVERIFY(touch(QStringLiteral("dest/a.txt"), QDateTime::currentDateTime().addSecs(3600)));

    SyncOptions options;
    options.skipNewer = true;
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);

    // Work done at the far end is not undone by a stale source. This is the
    // guard that makes a two-way workflow survivable.
    QCOMPARE(contentsOf(QStringLiteral("dest/a.txt")), QByteArray("done at the far end"));
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Skip), 1);
}

void TestSync::overwritesWhenAskedTo()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("from the source")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), QByteArray("older, shorter")));

    QVERIFY(touch(QStringLiteral("dest/a.txt"), QDateTime::currentDateTime().addSecs(-3600)));

    SyncOptions options;
    options.skipNewer = true;
    options.dryRun = false;

    QVERIFY(run(options));
    QCOMPARE(contentsOf(QStringLiteral("dest/a.txt")), QByteArray("from the source"));
}

void TestSync::comparesBySizeAlone()
{
    // Same size, different bytes, and timestamps that differ. Size-only is for
    // drives whose timestamps cannot be trusted, and it must ignore them.
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("aaaa")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), QByteArray("bbbb")));

    SyncOptions options;
    options.compare = SyncOptions::Compare::SizeOnly;
    options.skipNewer = false;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 0);
}

void TestSync::comparesByContents()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("aaaa")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), QByteArray("bbbb")));

    SyncOptions options;
    options.compare = SyncOptions::Compare::Contents;
    options.skipNewer = false;
    options.dryRun = false;

    QVERIFY(run(options));
    // Same size, so only a contents comparison catches it.
    QCOMPARE(contentsOf(QStringLiteral("dest/a.txt")), QByteArray("aaaa"));
}

void TestSync::toleratesSubSecondTimestampDrift()
{
    const QByteArray payload("identical");
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), payload));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/a.txt"), payload));

    const QDateTime when = QDateTime::currentDateTime();
    QVERIFY(touch(QStringLiteral("src/a.txt"), when));
    QVERIFY(touch(QStringLiteral("dest/a.txt"), when.addMSecs(400)));

    SyncOptions options;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);
    // Filesystems disagree about sub-second precision. Without slack, every sync
    // between two of them copies everything, every time.
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 0);
}

// ---------------------------------------------------------------- filters

void TestSync::filtersByPattern()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/keep.txt"), QByteArray("a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/skip.tmp"), QByteArray("b")));

    SyncOptions options;
    options.excludePatterns = { QStringLiteral("*.tmp") };
    options.dryRun = false;

    QVERIFY(run(options));
    QVERIFY(exists(QStringLiteral("dest/keep.txt")));
    QVERIFY(!exists(QStringLiteral("dest/skip.tmp")));
}

void TestSync::anIncludeBeatsABroadExclude()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/notes.tmp"), QByteArray("a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/other.tmp"), QByteArray("b")));

    SyncOptions options;
    options.excludePatterns = { QStringLiteral("*.tmp") };
    options.includePatterns = { QStringLiteral("notes.*") };
    options.dryRun = false;

    QVERIFY(run(options));
    // "Everything except .tmp, but definitely notes" is how people express this,
    // and it only works if the narrow rule wins.
    QVERIFY(exists(QStringLiteral("dest/notes.tmp")));
    QVERIFY(!exists(QStringLiteral("dest/other.tmp")));
}

void TestSync::aFilteredNameIsNeverDeletedByAMirror()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/build.tmp"), QByteArray("theirs")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.excludePatterns = { QStringLiteral("*.tmp") };
    options.dryRun = false;

    QVERIFY(run(options));
    // A name that was never considered for copying must not be deleted at the
    // far end -- that would act on a rule the user did not give.
    QVERIFY(exists(QStringLiteral("dest/build.tmp")));
}

// ---------------------------------------------------------------- dry run

void TestSync::aSourceThatCannotBeListedDeletesNothing()
{
    // The most destructive thing this application could do. A mirror works out
    // what to delete by asking what the source has; a listing that fails answers
    // "nothing", and the far end -- which may be the only remaining copy -- is
    // then emptied to match. The read that failed is a network hiccup, a
    // permission, a drive that went to sleep. It is not an instruction.
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/src/one.txt"), QByteArray("keep me"));
    memory->addFile(QStringLiteral("/src/two.txt"), QByteArray("me too"));
    memory->addFile(QStringLiteral("/dest/one.txt"), QByteArray("keep me"));
    memory->addFile(QStringLiteral("/dest/two.txt"), QByteArray("me too"));
    memory->addFile(QStringLiteral("/dest/three.txt"), QByteArray("and me"));
    memory->setFault(QStringLiteral("/src"), VfsError::NetworkError);

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = false;

    auto* task = new SyncTask(memory, VfsUri::fromString(QStringLiteral("mem:///src")), memory,
        VfsUri::fromString(QStringLiteral("mem:///dest")), options);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(task->plan().countOf(SyncPlan::Action::Delete), 0);
    for (const QString& name :
        { QStringLiteral("one.txt"), QStringLiteral("two.txt"), QStringLiteral("three.txt") }) {
        QVERIFY2(memory->stat(VfsUri::fromString(QStringLiteral("mem:///dest/") + name)).ok(),
            qPrintable(QStringLiteral("%1 was deleted because the source could not be read").arg(name)));
    }
    // And it says so, rather than reporting a mirror that did not happen.
    QVERIFY2(!task->failures().isEmpty(), "a sync that could not read the source has to say so");
    QCOMPARE(task->plan().unreadable().size(), 1);
}

void TestSync::oneUnreadableDirectoryDeletesNothingInsideIt()
{
    // The subtler form, and the likelier one: most of the source reads fine and
    // one directory in it does not. Everything else may be mirrored; the
    // corresponding directory at the far end must be left exactly as it is.
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/src/open/fine.txt"), QByteArray("copy me"));
    memory->addFile(QStringLiteral("/src/locked/hidden.txt"), QByteArray("cannot see"));
    memory->addFile(QStringLiteral("/dest/open/stale.txt"), QByteArray("should go"));
    memory->addFile(QStringLiteral("/dest/locked/precious.txt"), QByteArray("must stay"));
    memory->setFault(QStringLiteral("/src/locked"), VfsError::AccessDenied);

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = false;

    auto* task = new SyncTask(memory, VfsUri::fromString(QStringLiteral("mem:///src")), memory,
        VfsUri::fromString(QStringLiteral("mem:///dest")), options);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QVERIFY2(memory->stat(VfsUri::fromString(QStringLiteral("mem:///dest/locked/precious.txt"))).ok(),
        "nothing inside a directory the source could not show may be deleted");
    // The rest of the mirror still happened.
    QVERIFY(memory->stat(VfsUri::fromString(QStringLiteral("mem:///dest/open/fine.txt"))).ok());
    QVERIFY(!memory->stat(VfsUri::fromString(QStringLiteral("mem:///dest/open/stale.txt"))).ok());
    QCOMPARE(task->plan().unreadable(), QStringList { QStringLiteral("/src/locked") });
}

void TestSync::aSecondRunOverTheSameTreesDoesNothingAtAll()
{
    // The property that makes a sync worth scheduling. A second run that copies
    // everything again is a sync that never converges -- it costs the whole tree
    // every night and hides a real change in the noise.
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/deep/b.txt"), QByteArray("two")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/stale.txt"), QByteArray("old")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = false;

    SyncTask* first = run(options);
    QVERIFY(first);
    QVERIFY2(first->failures().isEmpty(), qPrintable(first->failures().join(QStringLiteral("; "))));
    QVERIFY(first->appliedCount() > 0);

    SyncTask* second = run(options);
    QVERIFY(second);
    QVERIFY2(second->failures().isEmpty(), qPrintable(second->failures().join(QStringLiteral("; "))));
    QCOMPARE(second->appliedCount(), 0);
    QCOMPARE(second->plan().countOf(SyncPlan::Action::Copy), 0);
    QCOMPARE(second->plan().countOf(SyncPlan::Action::Overwrite), 0);
    QCOMPARE(second->plan().countOf(SyncPlan::Action::Delete), 0);
}

void TestSync::sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe_data()
{
    QTest::addColumn<int>("compare");
    QTest::addColumn<bool>("shouldCopy");

    // Size-only misses it by design and says so in its name. The other two are
    // sold as telling files apart, and a file that differs in the middle while
    // keeping its length -- a database page, a patched binary, a corrupted block
    // -- is exactly the case somebody runs a checksum sync for.
    QTest::newRow("size only, and it misses it") << int(SyncOptions::Compare::SizeOnly) << false;
    QTest::newRow("contents, which is what it is for") << int(SyncOptions::Compare::Contents) << true;
}

void TestSync::sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe()
{
    QFETCH(int, compare);
    QFETCH(bool, shouldCopy);

    QVERIFY(m_tree->writeFile(QStringLiteral("src/page.bin"), QByteArray("AAAABBBBCCCC")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/page.bin"), QByteArray("AAAAXXXXCCCC")));
    const QDateTime when = QDateTime::currentDateTime().addSecs(-3600);
    QVERIFY(touch(QStringLiteral("src/page.bin"), when));
    QVERIFY(touch(QStringLiteral("dest/page.bin"), when));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Update;
    options.compare = static_cast<SyncOptions::Compare>(compare);
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(contentsOf(QStringLiteral("dest/page.bin")),
        shouldCopy ? QByteArray("AAAABBBBCCCC") : QByteArray("AAAAXXXXCCCC"));
}

void TestSync::aRenameIsNeverADeleteBeforeItIsACopy()
{
    // A file renamed at the source looks to a mirror like one name appearing and
    // another going away. Both are true, and the order they are done in is the
    // whole question: deleting first, on a run that then fails to copy, removes
    // the only copy there was.
    QVERIFY(m_tree->writeFile(QStringLiteral("src/after.txt"), QByteArray("the same bytes")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/before.txt"), QByteArray("the same bytes")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);

    int firstDelete = -1;
    int lastCopy = -1;
    const QList<SyncPlan::Step>& steps = task->plan().steps();
    for (int i = 0; i < steps.size(); ++i) {
        if (steps.at(i).action == SyncPlan::Action::Delete && firstDelete < 0)
            firstDelete = i;
        if (steps.at(i).action == SyncPlan::Action::Copy)
            lastCopy = i;
    }
    QVERIFY2(firstDelete >= 0, "the old name has to go eventually");
    QVERIFY2(lastCopy >= 0, "the new name has to arrive");
    QVERIFY2(lastCopy < firstDelete, "every copy is planned before any deletion");
}

void TestSync::aDryRunWritesNothing()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/stale.txt"), QByteArray("old")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);

    // The plan is real -- it is the same plan a live run would carry out -- and
    // nothing at all has happened.
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Copy), 1);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Delete), 1);
    QCOMPARE(task->appliedCount(), 0);
    QVERIFY(!exists(QStringLiteral("dest/a.txt")));
    QVERIFY(exists(QStringLiteral("dest/stale.txt")));
}

void TestSync::aDryRunAgainstADriveThatRefusesEveryWriteStillReportsNoFailures()
{
    // "Nothing appeared" is the weaker claim: a write that happened and failed
    // would leave nothing behind either. This one puts a drive underneath that
    // refuses every write and every delete, so a dry run that touched the
    // destination at all would have to report it.
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/deep/b.txt"), QByteArray("two")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/stale.txt"), QByteArray("old")));

    auto refusing = std::make_shared<FaultyFileSystem>(m_fs);
    refusing->writeFailsAt(0);
    refusing->removeFails();

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = true;

    auto* task = new SyncTask(m_fs, m_tree->rootUri().child(QStringLiteral("src")), refusing,
        m_tree->rootUri().child(QStringLiteral("dest")), options);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->appliedCount(), 0);
    // And it still worked out what it *would* do, which is the whole point.
    QVERIFY(task->plan().countOf(SyncPlan::Action::Copy) > 0);
    QVERIFY(task->plan().countOf(SyncPlan::Action::Delete) > 0);
    QVERIFY(exists(QStringLiteral("dest/stale.txt")));
    QVERIFY(!exists(QStringLiteral("dest/a.txt")));
}

void TestSync::directoriesArePlannedBeforeTheFilesInThem()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/deep/nested/file.txt"), QByteArray("x")));

    SyncOptions options;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);

    int lastDirectory = -1;
    int firstCopy = -1;
    // Held by value: plan() returns a temporary, and a reference into its
    // steps() dies at the end of the statement that took it.
    const SyncPlan plan = task->plan();
    const QList<SyncPlan::Step>& steps = plan.steps();
    for (int i = 0; i < steps.size(); ++i) {
        if (steps.at(i).action == SyncPlan::Action::CreateDirectory)
            lastDirectory = i;
        if (steps.at(i).action == SyncPlan::Action::Copy && firstCopy < 0)
            firstCopy = i;
    }

    // A copy into a folder that does not exist yet fails, so order is not
    // cosmetic here.
    QVERIFY(lastDirectory >= 0);
    QVERIFY(firstCopy > lastDirectory);
}

void TestSync::deletionsComeLast()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/a.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/gone.txt"), QByteArray("old")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);

    const SyncPlan plan = task->plan();
    const QList<SyncPlan::Step>& steps = plan.steps();
    QVERIFY(!steps.isEmpty());
    // A mirror that deleted first could remove a file it was about to be
    // handed back, and on a cancelled run that loss would be permanent.
    QCOMPARE(steps.last().action, SyncPlan::Action::Delete);
}

MOLE_TEST_MAIN(TestSync)
#include "tst_Sync.moc"
