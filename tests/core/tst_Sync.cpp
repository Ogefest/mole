#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/sync/SyncTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

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

    void aDryRunWritesNothing();
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
