#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/duplicates/ContentComparison.h"
#include "core/sync/SyncTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QFile>
#include <QFileInfo>

#include <filesystem>

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
    void twoFilesDifferingOnlyInTheirLastChunkAreSeenAsDifferent();
    void twoIdenticalFilesAreLeftAloneAndNothingIsCopied();
    void aFileThatCannotBeOpenedIsNotReportedAsDiffering();
    void toleratesSubSecondTimestampDrift();

    void filtersByPattern();
    void anIncludeBeatsABroadExclude();
    void aFilteredNameIsNeverDeletedByAMirror();

    void aSourceThatCannotBeListedDeletesNothing();
    void oneUnreadableDirectoryDeletesNothingInsideIt();
    void oneUnreadableDirectoryOnTheDiskDeletesNothingInsideItEither();
    void aSecondRunOverTheSameTreesDoesNothingAtAll();
    void sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe_data();
    void sameSizeAndDifferentContentIsSeenOnlyWhereItCanBe();
    void aRenameIsNeverADeleteBeforeItIsACopy();

    void aNameThatIsAFileOnOneSideAndAFolderOnTheOtherIsSkipped_data();
    void aNameThatIsAFileOnOneSideAndAFolderOnTheOtherIsSkipped();
    void aTypeMismatchIsSkippedByAMirrorToo();

    void aDirectoryLoopIsAFinitePlanThatNamesTheLink();
    void aLinkIsPlannedAsALinkAndNotWalkedIntoAtAll();

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
    /// Why the plan says it would touch this file, or an empty string when it
    /// would not touch it at all. The reason is half of what a contents
    /// comparison answers: "differs" and "could not be read" are both a copy,
    /// and only the words tell them apart.
    QString reasonFor(const SyncPlan& plan, const QString& relative) const;

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

QString TestSync::reasonFor(const SyncPlan& plan, const QString& relative) const
{
    for (const SyncPlan::Step& step : plan.steps()) {
        if (step.relativePath == relative)
            return step.reason;
    }
    return {};
}

// ---- what a contents comparison answers --------------------------------
//
// Sync was the one place left in the codebase that decided two files were the
// same by hashing both of them whole with SHA-256. ADR-0046 replaced that shape
// in the duplicates scan: Qt 6.4 carries its own SHA-2 and does not use the
// processor's SHA-NI instructions, so hashing runs at about 218 MB/s whatever the
// storage is, against 87 GB/s for a memcmp -- and a comparison stops at the first
// chunk that differs, where a hash always reads both files to the end. See
// MOLE-215. What has to survive the change is what the three answers mean.

void TestSync::twoFilesDifferingOnlyInTheirLastChunkAreSeenAsDifferent()
{
    // Two chunks and a bit, identical until the very last byte: the case a
    // comparison that stopped early, or that compared only a head, gets wrong --
    // and the one a hash could never get wrong, which is what makes it the
    // assertion worth having here.
    QByteArray body(static_cast<int>(kComparisonChunkBytes * 2 + 4096), 'a');
    QByteArray edited = body;
    edited[edited.size() - 1] = 'b';
    QVERIFY(m_tree->writeFile(QStringLiteral("src/page.bin"), body));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/page.bin"), edited));

    SyncOptions options;
    options.compare = SyncOptions::Compare::Contents;
    options.skipNewer = false;
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 1);
    QCOMPARE(reasonFor(task->plan(), QStringLiteral("page.bin")), QStringLiteral("contents differ"));
    QCOMPARE(contentsOf(QStringLiteral("dest/page.bin")), body);
}

void TestSync::twoIdenticalFilesAreLeftAloneAndNothingIsCopied()
{
    // Larger than one chunk again, so the comparison has to run to the end of both
    // to say so -- the answer a lockstep read gives only by agreeing all the way.
    const QByteArray body(static_cast<int>(kComparisonChunkBytes + 1024), 'z');
    QVERIFY(m_tree->writeFile(QStringLiteral("src/same.bin"), body));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/same.bin"), body));

    SyncOptions options;
    options.compare = SyncOptions::Compare::Contents;
    options.skipNewer = false;
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 0);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Copy), 0);
    QCOMPARE(task->appliedCount(), 0);
}

void TestSync::aFileThatCannotBeOpenedIsNotReportedAsDiffering()
{
    // The one edge that decides whether this was a clean change or a silent one.
    // partitionByContents() leaves a file it could not open out of its result
    // altogether -- deliberately, because an unreadable file is not a match for
    // every other unreadable file -- so a careless reading of the return value
    // gives "contents differ" for a file nobody could look at. That is not a
    // cosmetic difference in a sync: it is the difference between reporting a
    // file that could not be read and quietly overwriting the other side with
    // whatever was there.
    const QByteArray body(4096, 'a');
    QByteArray other(4096, 'a');
    other[0] = 'b';
    QVERIFY(m_tree->writeFile(QStringLiteral("src/secret.bin"), body));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/secret.bin"), other));

    const QString locked = m_tree->absolute(QStringLiteral("src/secret.bin"));
    // Root, or a filesystem that does not enforce permissions, can still open it,
    // and then there is nothing here to prove. This is where that guard was
    // written; it is shared now, because three other cases needed it.
    if (!madeUnreadable(locked))
        QSKIP("this account can read a file with no permissions at all");

    SyncOptions options;
    options.compare = SyncOptions::Compare::Contents;
    options.skipNewer = false;
    options.dryRun = true;

    SyncTask* task = run(options);
    // Left readable again before any assertion, so a failure below does not leave
    // a directory the fixture cannot clean up.
    QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner);

    QVERIFY(task);
    const QString reason = reasonFor(task->plan(), QStringLiteral("secret.bin"));
    QCOMPARE(reason, QStringLiteral("could not be compared"));
    QVERIFY2(reason != QStringLiteral("contents differ"),
        "a file nobody could open must not be reported as one that differs");
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

/// The same claim against the disk, which is where it was not true.
///
/// The memory drive above answers a fault for a directory nobody may read, and
/// every "unreadable directory" case in the tree was written against it. The
/// local backend answered an empty, successful listing instead -- QDirIterator
/// on a directory it cannot open yields nothing, and nothing came back as
/// "there is nothing in it". A mirror believes that: the folder at the far end
/// is emptied to match a source folder that was never read. See MOLE-333.
void TestSync::oneUnreadableDirectoryOnTheDiskDeletesNothingInsideItEither()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    QVERIFY(m_tree->writeFile(QStringLiteral("src/open/fine.txt"), QByteArray("copy me")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/locked/hidden.txt"), QByteArray("cannot see")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/open/stale.txt"), QByteArray("should go")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/locked/precious.txt"), QByteArray("must stay")));
    if (!madeUnreadable(m_tree->absolute(QStringLiteral("src/locked"))))
        QSKIP("this account can list a directory with no permissions at all");

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;
    options.dryRun = false;

    SyncTask* task = run(options);
    // Put back before any assertion, so a failure still leaves a tree that can
    // be deleted.
    QFile::setPermissions(m_tree->absolute(QStringLiteral("src/locked")),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QVERIFY(task);

    QVERIFY2(exists(QStringLiteral("dest/locked/precious.txt")),
        "nothing inside a directory the source could not show may be deleted");
    QVERIFY(exists(QStringLiteral("dest/open/fine.txt")));
    QVERIFY(!exists(QStringLiteral("dest/open/stale.txt")));
    QCOMPARE(task->plan().unreadable().size(), 1);
    QVERIFY2(task->plan().unreadable().first().endsWith(QStringLiteral("/src/locked")),
        qPrintable(task->plan().unreadable().first()));
#endif
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
    // By value: plan() hands back a copy, so a reference into it would outlive
    // what it refers to by the length of one statement.
    const QList<SyncPlan::Step> steps = task->plan().steps();
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

// ------------------------------------------------------------------- links

/// The two sides disagreeing about what a name *is*.
///
/// Neither direction used to be noticed. A source file `x` against a destination
/// folder `x` was compared by size -- a folder measures 0 -- so the plan said
/// "size differs" and asked for an Overwrite; the local backend then removed the
/// folder, which succeeds when it is empty, and put the file in its place. The
/// other way round, a source folder `x` against a destination file `x` planned
/// no CreateDirectory because the name was taken, listed a file as a directory
/// (NotADirectory, read as empty), planned every child as a Copy, and every one
/// of them failed at openWrite. Either way the dry run did not predict the run,
/// which is the one thing a dry run is for.
void TestSync::aNameThatIsAFileOnOneSideAndAFolderOnTheOtherIsSkipped_data()
{
    QTest::addColumn<bool>("sourceIsTheFolder");
    QTest::addColumn<bool>("dryRun");
    QTest::addColumn<QString>("reason");

    const QString folderThere = QStringLiteral("a file in the source and a folder at the destination");
    const QString fileThere = QStringLiteral("a folder in the source and a file at the destination");

    QTest::newRow("a file onto a folder") << false << false << folderThere;
    QTest::newRow("a file onto a folder, dry run") << false << true << folderThere;
    QTest::newRow("a folder onto a file") << true << false << fileThere;
    QTest::newRow("a folder onto a file, dry run") << true << true << fileThere;
}

void TestSync::aNameThatIsAFileOnOneSideAndAFolderOnTheOtherIsSkipped()
{
    QFETCH(bool, sourceIsTheFolder);
    QFETCH(bool, dryRun);
    QFETCH(QString, reason);

    if (sourceIsTheFolder) {
        QVERIFY(m_tree->writeFile(QStringLiteral("src/report/page.txt"), QByteArray("in the folder")));
        QVERIFY(m_tree->writeFile(QStringLiteral("dest/report"), QByteArray("a file at the far end")));
    } else {
        QVERIFY(m_tree->writeFile(QStringLiteral("src/report"), QByteArray("a file in the source")));
        QVERIFY(m_tree->writeFile(QStringLiteral("dest/report/page.txt"), QByteArray("in the folder")));
    }
    // A neighbour that has nothing wrong with it, so the assertion is that the
    // mismatch was skipped rather than that the whole run gave up.
    QVERIFY(m_tree->writeFile(QStringLiteral("src/notes.txt"), QByteArray("ordinary")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Update;
    options.dryRun = dryRun;

    SyncTask* task = run(options);
    QVERIFY(task);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    QCOMPARE(reasonFor(task->plan(), QStringLiteral("report")), reason);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 0);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::CreateDirectory), 0);
    // The folder's children are not planned either: walking into a name that is
    // a file at the other end is what produced a copy per child that could not
    // land anywhere.
    QCOMPARE(reasonFor(task->plan(), QStringLiteral("report/page.txt")), QString());

    // Both sides are exactly as they were, in a live run as much as in a dry one.
    if (sourceIsTheFolder) {
        QCOMPARE(contentsOf(QStringLiteral("dest/report")), QByteArray("a file at the far end"));
        QVERIFY(exists(QStringLiteral("src/report/page.txt")));
    } else {
        QCOMPARE(contentsOf(QStringLiteral("dest/report/page.txt")), QByteArray("in the folder"));
        QVERIFY(QFileInfo(m_tree->absolute(QStringLiteral("dest/report"))).isDir());
    }

    // And the neighbour went, unless nothing was meant to go anywhere.
    QCOMPARE(exists(QStringLiteral("dest/notes.txt")), !dryRun);
}

/// A mirror does not get to resolve it by deleting either.
///
/// It is the mode with a deletion already in hand, so it is the one where
/// "delete what is there and copy over it" looks like the obvious answer. It is
/// not: a plan is sorted with deletions last, so that a mirror never removes a
/// file it is about to be given back, and a delete-then-copy pair for one name
/// would be carried out in the wrong order. The name is left alone and the plan
/// says why -- which the run then reports, rather than a failure per child.
void TestSync::aTypeMismatchIsSkippedByAMirrorToo()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("src/report"), QByteArray("a file in the source")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/report/page.txt"), QByteArray("keep me")));

    SyncOptions options;
    options.mode = SyncOptions::Mode::Mirror;

    SyncTask* task = run(options);
    QVERIFY(task);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    QCOMPARE(task->plan().countOf(SyncPlan::Action::Delete), 0);
    QCOMPARE(task->plan().countOf(SyncPlan::Action::Overwrite), 0);
    QCOMPARE(reasonFor(task->plan(), QStringLiteral("report")),
        QStringLiteral("a file in the source and a folder at the destination"));
    QCOMPARE(contentsOf(QStringLiteral("dest/report/page.txt")), QByteArray("keep me"));
}

void TestSync::aDirectoryLoopIsAFinitePlanThatNamesTheLink()
{
    // src/inside/back -> src, so following links means planning a CreateDirectory
    // at every level until the kernel refuses the path. The plan has to be finite
    // and it has to say the link is there. See ADR-0092.
    QVERIFY(m_tree->makeDirs(QStringLiteral("src/inside")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/inside/leaf.txt"), QByteArray("a leaf")));
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("src")), m_tree->absolute(QStringLiteral("src/inside/back"))));

    SyncOptions options;
    options.dryRun = true;

    SyncTask* task = run(options);
    QVERIFY(task);

    const SyncPlan plan = task->plan();
    // Every step's path is one of the four things in the tree. A walk that went
    // through the link produces "inside/back/inside/back/..." and hundreds of
    // steps; the count is what says it stopped.
    QVERIFY2(plan.steps().size() <= 4, qPrintable(QStringLiteral("%1 steps").arg(plan.steps().size())));

    QStringList paths;
    for (const SyncPlan::Step& step : plan.steps())
        paths.append(step.relativePath);
    QVERIFY2(paths.contains(QStringLiteral("inside/back")), qPrintable(paths.join(QStringLiteral(", "))));
    for (const QString& path : paths)
        QVERIFY2(!path.contains(QStringLiteral("back/inside")), qPrintable(path));
}

void TestSync::aLinkIsPlannedAsALinkAndNotWalkedIntoAtAll()
{
    // The same rule a copy follows: the link arrives as a link, pointing where it
    // pointed, and nothing is read through it.
    QVERIFY(m_tree->makeDirs(QStringLiteral("src")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/real.bin"), QByteArray("the real thing")));
    QVERIFY(QFile::link(QStringLiteral("real.bin"), m_tree->absolute(QStringLiteral("src/near.bin"))));

    SyncOptions options;
    options.dryRun = false;

    SyncTask* task = run(options);
    QVERIFY(task);
    QCOMPARE(task->failures(), QStringList());

    const QFileInfo arrived(m_tree->absolute(QStringLiteral("dest/near.bin")));
    QVERIFY2(arrived.isSymLink(), "a link must arrive as a link");
    QCOMPARE(QString::fromStdString(
                 std::filesystem::read_symlink(arrived.absoluteFilePath().toStdString()).string()),
        QStringLiteral("real.bin"));
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
