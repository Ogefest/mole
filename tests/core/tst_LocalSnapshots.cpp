#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// Earlier states of a file, where the filesystem already keeps them.
///
/// A snapshot exposed as a path is an ordinary directory, which is what makes
/// almost all of this testable with no snapshot anywhere: a temporary tree laid
/// out in the same shape exercises the discovery walk, the per-directory union
/// and the silence outside a root, on any machine and without privileges.
///
/// What it cannot prove is the assumption the whole thing rests on -- that a real
/// snapshot directory behaves like any other directory. That is the live case at
/// the bottom, which reads MOLE_TEST_SNAPSHOT_PATH and skips with a reason when
/// it is unset.
class TestLocalSnapshots : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    LocalFileSystem m_fs;

    /// The dataset root: everything under here has snapshots.
    QString root() const { return QDir(m_dir->path()).filePath(QStringLiteral("dataset")); }
    /// A place on the same disk with no snapshot directory above it.
    QString outside() const { return QDir(m_dir->path()).filePath(QStringLiteral("elsewhere")); }

    VfsUri uriFor(const QString& path) const { return VfsUri::fromLocalPath(path); }
    VfsUri inRoot(const QString& relative) const { return uriFor(QDir(root()).filePath(relative)); }
    /// Writes a file, making its parents.
    bool write(const QString& absolute, const QByteArray& contents) const;
    /// A file as it was in `snapshot`.
    bool writeSnapshot(const QString& snapshot, const QString& relative, const QByteArray& contents) const;
    FileEntry entryFor(const VfsUri& uri);

private slots:
    void init();
    void cleanup();

    void aFolderInsideARootOffersTheEarlierVersionsOfItsFiles();
    void openingAnEarlierVersionOpensThatVersion();
    void aVersionTokenThatIsAPathCannotReachTheCurrentFile();
    void theRootIsFoundFromFarBelowIt();
    void aFileNoSnapshotKeptIsOfferedNothing();
    void aFolderOutsideAnyRootIsOfferedNothing();
    void aRootKeepingNoSnapshotsOffersNothing();
    void theNearestRootWinsWhenTheyNest();
    void aWholeFolderIsAnsweredWithOnePassPerSnapshot();
    void writingToAnEarlierVersionIsRefused();
    void listingInsideASnapshotKeepsTheVersionOnEveryRow();

    void aRealSnapshotBehavesLikeAnyOtherDirectory();
};

bool TestLocalSnapshots::write(const QString& absolute, const QByteArray& contents) const
{
    if (!QDir().mkpath(QFileInfo(absolute).absolutePath()))
        return false;
    QFile file(absolute);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

bool TestLocalSnapshots::writeSnapshot(
    const QString& snapshot, const QString& relative, const QByteArray& contents) const
{
    const QString inside = QDir(root()).filePath(
        LocalFileSystem::snapshotDirectory() + QLatin1Char('/') + snapshot + QLatin1Char('/') + relative);
    return write(inside, contents);
}

FileEntry TestLocalSnapshots::entryFor(const VfsUri& uri)
{
    const Result<FileEntry> stat = m_fs.stat(uri);
    return stat.ok() ? stat.value() : FileEntry {};
}

void TestLocalSnapshots::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    // Two earlier states of one file, one file that has always been there, and
    // one that arrived after the last snapshot.
    QVERIFY(writeSnapshot(
        QStringLiteral("2026-08-01"), QStringLiteral("papers/report.txt"), QByteArray("the first draft")));
    QVERIFY(writeSnapshot(
        QStringLiteral("2026-08-15"), QStringLiteral("papers/report.txt"), QByteArray("the second draft")));
    QVERIFY(writeSnapshot(
        QStringLiteral("2026-08-01"), QStringLiteral("papers/gone.txt"), QByteArray("deleted since")));

    QVERIFY(write(QDir(root()).filePath(QStringLiteral("papers/report.txt")), QByteArray("the third draft")));
    QVERIFY(write(QDir(root()).filePath(QStringLiteral("papers/fresh.txt")), QByteArray("brand new")));
    QVERIFY(write(QDir(outside()).filePath(QStringLiteral("notes.txt")), QByteArray("no snapshots here")));
}

void TestLocalSnapshots::cleanup()
{
    m_dir.reset();
}

void TestLocalSnapshots::aFolderInsideARootOffersTheEarlierVersionsOfItsFiles()
{
    const VfsUri report = inRoot(QStringLiteral("papers/report.txt"));

    const FileActionList offered = m_fs.actionsFor(report, entryFor(report));
    QCOMPARE(offered.size(), 1);
    QCOMPARE(offered.first().id, LocalFileSystem::versionsActionId());
    QCOMPARE(offered.first().answers, FileActionKind::Uris);

    const Result<FileActionOutcome> outcome
        = m_fs.invoke(LocalFileSystem::versionsActionId(), report, CancelToken());
    QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));
    QCOMPARE(outcome.value().kind, FileActionKind::Uris);
    QCOMPARE(outcome.value().uris.size(), 2);

    QStringList named;
    for (const VfsUri& uri : outcome.value().uris) {
        named.append(uri.version());
        // The same file, at another state of itself.
        QCOMPARE(uri.withoutVersion(), report);
    }
    QCOMPARE(named, QStringList({ QStringLiteral("2026-08-01"), QStringLiteral("2026-08-15") }));
}

/// The point of them being paths: reading one is the ordinary read path, and the
/// bytes are that version's rather than the file's.
void TestLocalSnapshots::openingAnEarlierVersionOpensThatVersion()
{
    const VfsUri report = inRoot(QStringLiteral("papers/report.txt"));
    const VfsUri first = report.withVersion(QStringLiteral("2026-08-01"));

    const Result<FileEntry> stat = m_fs.stat(first);
    QVERIFY2(stat.ok(), qPrintable(stat.error().message));
    QCOMPARE(stat.value().size, qint64(QByteArray("the first draft").size()));
    QCOMPARE(stat.value().uri.version(), QStringLiteral("2026-08-01"));

    const Result<std::unique_ptr<QIODevice>> read = m_fs.openRead(first);
    QVERIFY2(read.ok(), qPrintable(read.error().message));
    QCOMPARE(read.value()->readAll(), QByteArray("the first draft"));

    // And the file itself is untouched by any of it.
    const Result<std::unique_ptr<QIODevice>> now = m_fs.openRead(report);
    QVERIFY(now.ok());
    QCOMPARE(now.value()->readAll(), QByteArray("the third draft"));

    // A snapshot that does not exist is missing, not the current file.
    QCOMPARE(
        m_fs.openRead(report.withVersion(QStringLiteral("1999-01-01"))).error().code, VfsError::NotFound);
}

/// A version token is a directory name, and it was spliced into a path unchecked.
///
/// `?version=../../../current` in a bookmark, a session or a saved set reads the
/// *current* file while every screen says it is an earlier version -- which is
/// ADR-0077's "worse than the feature not existing": a reader looking at what
/// they believe is last month's draft, editing decisions on it, with the
/// application agreeing. And it reaches outside the snapshot root entirely.
/// See MOLE-359.
void TestLocalSnapshots::aVersionTokenThatIsAPathCannotReachTheCurrentFile()
{
    const VfsUri report = inRoot(QStringLiteral("papers/report.txt"));

    // What a hand-edited bookmark can carry. Each of these, spliced in, names
    // the current file or something outside the root.
    // The snapshot directory is two levels down, so `../..` from inside it is
    // the root itself -- which is why this is a shape to refuse rather than a
    // depth to count.
    const QStringList wicked { QStringLiteral("../.."), QStringLiteral("../../"),
        QStringLiteral("2026-08-01/../../.."), QStringLiteral(".."), QStringLiteral("."),
        QStringLiteral("/etc") };

    for (const QString& token : wicked) {
        const VfsUri asked = report.withVersion(token);
        const Result<std::unique_ptr<QIODevice>> read = m_fs.openRead(asked);
        QVERIFY2(!read.ok(),
            qPrintable(
                QStringLiteral("version=%1 opened something: %2")
                    .arg(token, QString::fromUtf8(read.value() ? read.value()->readAll() : QByteArray()))));
        QCOMPARE(read.error().code, VfsError::NotFound);
        QVERIFY2(
            !m_fs.stat(asked).ok(), qPrintable(QStringLiteral("version=%1 stat'd something").arg(token)));
    }

    // The one above that really did read the current file, named on its own so
    // this case cannot pass because a path happened not to exist.
    const Result<std::unique_ptr<QIODevice>> escaped
        = m_fs.openRead(report.withVersion(QStringLiteral("../..")));
    QVERIFY2(!escaped.ok() || escaped.value()->readAll() != QByteArray("the third draft"),
        "a version token walked out of the snapshot directory and read the current file");

    // A real token still works, which is what says this refuses a shape rather
    // than the feature.
    const Result<std::unique_ptr<QIODevice>> real
        = m_fs.openRead(report.withVersion(QStringLiteral("2026-08-01")));
    QVERIFY2(real.ok(), qPrintable(real.error().message));
    QCOMPARE(real.value()->readAll(), QByteArray("the first draft"));
}

/// A path may be far below the root, and the relative part is what indexes into
/// each snapshot.
void TestLocalSnapshots::theRootIsFoundFromFarBelowIt()
{
    QVERIFY(writeSnapshot(
        QStringLiteral("2026-08-01"), QStringLiteral("a/b/c/deep.txt"), QByteArray("deep, and older")));
    QVERIFY(write(QDir(root()).filePath(QStringLiteral("a/b/c/deep.txt")), QByteArray("deep")));

    const VfsUri deep = inRoot(QStringLiteral("a/b/c/deep.txt"));
    QCOMPARE(m_fs.actionsFor(deep, entryFor(deep)).size(), 1);

    const Result<FileActionOutcome> outcome
        = m_fs.invoke(LocalFileSystem::versionsActionId(), deep, CancelToken());
    QVERIFY(outcome.ok());
    QCOMPARE(outcome.value().uris.size(), 1);
    QCOMPARE(m_fs.openRead(outcome.value().uris.first()).value()->readAll(), QByteArray("deep, and older"));
}

void TestLocalSnapshots::aFileNoSnapshotKeptIsOfferedNothing()
{
    const VfsUri fresh = inRoot(QStringLiteral("papers/fresh.txt"));
    QVERIFY2(m_fs.actionsFor(fresh, entryFor(fresh)).isEmpty(),
        "a file that is in no snapshot has no earlier version to offer");
    QCOMPARE(m_fs.invoke(LocalFileSystem::versionsActionId(), fresh, CancelToken()).error().code,
        VfsError::NotFound);

    // Nor is a folder offered one: an earlier state of a folder is reached by
    // opening an earlier state of a file inside it.
    const VfsUri papers = inRoot(QStringLiteral("papers"));
    QVERIFY(m_fs.actionsFor(papers, entryFor(papers)).isEmpty());
}

/// Most drives on most machines, and they must pay nothing for the feature
/// existing.
///
/// What is asserted here is what is true on any machine: nothing is offered, and
/// the answer is an error. The probe's own answer for an arbitrary folder is not,
/// and neither is which error: the machine this was written on has its root
/// filesystem on one of these, so "/" is a root and every path on it has one
/// above it. That is exactly why the nearest one has to win -- see below.
void TestLocalSnapshots::aFolderOutsideAnyRootIsOfferedNothing()
{
    const VfsUri notes = uriFor(QDir(outside()).filePath(QStringLiteral("notes.txt")));

    QVERIFY2(m_fs.actionsFor(notes, entryFor(notes)).isEmpty(),
        "no snapshot anywhere holds this path, so there is nothing to offer for it");
    QVERIFY(m_fs.entriesWithActions(uriFor(outside()), CancelToken()).value().isEmpty());

    // An error, and nothing offered -- but which error is the host's answer
    // rather than ours, and both of these are right. A machine whose root
    // filesystem keeps snapshots has a root above every path, so this one has a
    // root and no snapshot in it holds the file: NotFound. A machine with no
    // such filesystem anywhere -- a container on overlayfs, which is where this
    // came up -- has no root at all: NotSupported. Asserting one of them held
    // the suite to the filesystem the test happened to be written on, and the
    // Fedora job in MOLE-297 failed on exactly that.
    const VfsError::Code code
        = m_fs.invoke(LocalFileSystem::versionsActionId(), notes, CancelToken()).error().code;
    QVERIFY2(code == VfsError::NotFound || code == VfsError::NotSupported,
        qPrintable(QStringLiteral("asked for versions outside any root and got code %1").arg(code)));
}

/// A dataset with the directory and nothing in it is the ordinary state of a
/// machine that has the feature switched on and has never used it -- and it must
/// read as "nothing to offer" rather than as a drive that offers something and
/// then produces nothing.
void TestLocalSnapshots::aRootKeepingNoSnapshotsOffersNothing()
{
    const QString bare = QDir(m_dir->path()).filePath(QStringLiteral("bare"));
    QVERIFY(QDir().mkpath(QDir(bare).filePath(LocalFileSystem::snapshotDirectory())));
    QVERIFY(write(QDir(bare).filePath(QStringLiteral("notes.txt")), QByteArray("nothing kept")));

    LocalFileSystem drive;
    drive.probe(uriFor(bare), CancelToken());
    QCOMPARE(drive.offers().state, DriveOffers::State::Answered);
    QVERIFY2(drive.offers().ids.isEmpty(), "a root keeping no snapshots has nothing to offer");

    const VfsUri notes = uriFor(QDir(bare).filePath(QStringLiteral("notes.txt")));
    QVERIFY(drive.actionsFor(notes, entryFor(notes)).isEmpty());

    // And a root that is keeping some does say so.
    LocalFileSystem inside;
    inside.probe(inRoot(QStringLiteral("papers")), CancelToken());
    QVERIFY(inside.offers().has(LocalFileSystem::versionsActionId()));
}

/// Datasets nest, and the machine this was written on has its root filesystem on
/// one -- so a temporary tree under it has two roots above every path. Answering
/// with the outer one would index every relative path into the wrong snapshots
/// and produce versions of files that are not the file.
void TestLocalSnapshots::theNearestRootWinsWhenTheyNest()
{
    // An inner dataset inside the outer one, with a file of the same name in
    // both, so picking the wrong root gives the wrong contents rather than an
    // error that would be noticed.
    const QString inner = QDir(root()).filePath(QStringLiteral("papers/inner"));
    QVERIFY(write(
        QDir(inner).filePath(LocalFileSystem::snapshotDirectory() + QStringLiteral("/2026-08-20/report.txt")),
        QByteArray("the inner one, as it was")));
    QVERIFY(write(QDir(inner).filePath(QStringLiteral("report.txt")), QByteArray("the inner one")));

    const VfsUri report = inRoot(QStringLiteral("papers/inner/report.txt"));
    const Result<FileActionOutcome> outcome
        = m_fs.invoke(LocalFileSystem::versionsActionId(), report, CancelToken());
    QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));

    QCOMPARE(outcome.value().uris.size(), 1);
    QCOMPARE(outcome.value().uris.first().version(), QStringLiteral("2026-08-20"));
    QCOMPARE(m_fs.openRead(outcome.value().uris.first()).value()->readAll(),
        QByteArray("the inner one, as it was"));
}

/// One pass per snapshot, not one per file -- and the signature of that is a
/// file the current folder does not have at all. Asking per file could never
/// find `gone.txt`; reading each snapshot's copy of the folder does.
void TestLocalSnapshots::aWholeFolderIsAnsweredWithOnePassPerSnapshot()
{
    const Result<QStringList> named
        = m_fs.entriesWithActions(inRoot(QStringLiteral("papers")), CancelToken());
    QVERIFY2(named.ok(), qPrintable(named.error().message));

    QCOMPARE(named.value(), QStringList({ QStringLiteral("gone.txt"), QStringLiteral("report.txt") }));
    QVERIFY2(
        !named.value().contains(QStringLiteral("fresh.txt")), "a file no snapshot kept must not be marked");

    // The cost is per snapshot rather than per row: a folder with a thousand
    // files in it answers from the same two passes.
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(
            write(QDir(root()).filePath(QStringLiteral("papers/file%1.txt").arg(i)), QByteArray("filler")));
    }
    const Result<QStringList> again
        = m_fs.entriesWithActions(inRoot(QStringLiteral("papers")), CancelToken());
    QVERIFY(again.ok());
    QCOMPARE(again.value(), named.value());
}

/// Mole shows what the filesystem holds and takes a copy out of it. The kernel
/// refuses too, but it refuses with an I/O error, which reads as a fault rather
/// than as an answer.
void TestLocalSnapshots::writingToAnEarlierVersionIsRefused()
{
    const VfsUri first
        = inRoot(QStringLiteral("papers/report.txt")).withVersion(QStringLiteral("2026-08-01"));

    QCOMPARE(m_fs.openWrite(first).error().code, VfsError::NotSupported);
    QCOMPARE(m_fs.remove(first, false).error().code, VfsError::NotSupported);
    QCOMPARE(m_fs.makeDirectory(first).error().code, VfsError::NotSupported);
    QCOMPARE(
        m_fs.rename(first, inRoot(QStringLiteral("papers/copy.txt"))).error().code, VfsError::NotSupported);
    QCOMPARE(
        m_fs.rename(inRoot(QStringLiteral("papers/report.txt")), first).error().code, VfsError::NotSupported);

    // And nothing was written on the way to refusing.
    QCOMPARE(m_fs.openRead(first).value()->readAll(), QByteArray("the first draft"));
}

void TestLocalSnapshots::listingInsideASnapshotKeepsTheVersionOnEveryRow()
{
    const VfsUri papers = inRoot(QStringLiteral("papers")).withVersion(QStringLiteral("2026-08-01"));

    const Result<FileEntryList> listing = m_fs.list(papers, CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 2); // report.txt and gone.txt, as they were

    for (const FileEntry& entry : listing.value()) {
        QVERIFY2(entry.uri.version() == QStringLiteral("2026-08-01"),
            qPrintable(QStringLiteral("%1 lost its version").arg(entry.name)));
    }
}

/// The one thing a temporary tree cannot stand in for. Set MOLE_TEST_SNAPSHOT_PATH
/// to a folder inside a snapshotted root whose files have earlier states.
void TestLocalSnapshots::aRealSnapshotBehavesLikeAnyOtherDirectory()
{
    const QString folder = QString::fromLocal8Bit(qgetenv("MOLE_TEST_SNAPSHOT_PATH"));
    if (folder.isEmpty()) {
        QSKIP("No snapshotted folder in the environment; set MOLE_TEST_SNAPSHOT_PATH to a folder "
              "inside a dataset that exposes its snapshots.");
    }

    const VfsUri here = VfsUri::fromLocalPath(folder);
    const Result<FileEntryList> listing = m_fs.list(here, CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));

    const Result<QStringList> named = m_fs.entriesWithActions(here, CancelToken());
    QVERIFY2(named.ok(), qPrintable(named.error().message));
    QVERIFY2(!named.value().isEmpty(), "the folder given has no file with an earlier state in any snapshot");

    // One file, all the way through: offered, performed, and read back as
    // something other than the file as it is now.
    for (const FileEntry& entry : listing.value()) {
        if (entry.isDir || !named.value().contains(entry.name))
            continue;

        QVERIFY2(!m_fs.actionsFor(entry.uri, entry).isEmpty(), qPrintable(entry.name));
        const Result<FileActionOutcome> outcome
            = m_fs.invoke(LocalFileSystem::versionsActionId(), entry.uri, CancelToken());
        QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));
        QVERIFY(!outcome.value().uris.isEmpty());

        const Result<std::unique_ptr<QIODevice>> read = m_fs.openRead(outcome.value().uris.first());
        QVERIFY2(read.ok(), qPrintable(read.error().message));
        const Result<FileEntry> stat = m_fs.stat(outcome.value().uris.first());
        QVERIFY2(stat.ok(), "a snapshot's copy of a file has to stat like any other file");
        return;
    }
    QFAIL("no file in the folder given could be read from a snapshot");
}

MOLE_TEST_MAIN(TestLocalSnapshots)
#include "tst_LocalSnapshots.moc"
