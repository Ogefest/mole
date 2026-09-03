#include "support/MoleTestMain.h"
#include "support/SqliteFaults.h"
#include "support/TestSupport.h"

#include "core/index/IndexDatabase.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <thread>
#include <vector>

using namespace mole;
using namespace mole::test;

class TestIndexDatabase : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void opensAndCreatesSchema();
    void reopeningIsIdempotent();
    void upsertVolumeReturnsSameIdForSameRoot();
    void insertBatchStoresRows();
    void aWriteRefusedByAnotherConnectionSaysTheIndexWasLocked();
    void aWriteThatRanOutOfRoomSaysWhatHappenedRatherThanNothing();
    void searchMatchesSubstring();
    void searchIsCaseInsensitiveForNonAsciiByDefault();
    void searchHonoursCaseSensitiveFlag();
    void searchFiltersByExtensionAndKind();
    void searchFiltersBySize();
    void searchScopesToVolume();
    void searchRespectsLimit();
    void searchReturnsResolvableUris();
    void rowsFromAnUnfinishedScanAreNotVisible();
    void anAbandonedScanLeavesThePreviousContents();
    void abandoningTheVolumesOwnScanDoesNothing();
    void aScanNothingFinishedIsSweptUpByTheNext();
    void aRescanTouchesOnlyItsOwnVolume();
    void removeVolumeDropsEverything();
    void commitScanRecordsTheCountAndTheTime();
    void anIndexWrittenBeforeGenerationsStaysVisible();
    void whatAFileSaysAboutItselfIsStoredAndAskedFor();
    void aRowInsideAContainerKeepsItsOwnAddress();
    void aFolderScopedSearchFindsWhatIsInsideAnArchiveInThatFolder();
    void carryingASubtreeForwardIntoALockedIndexSaysSoRatherThanCarryingNothing();
    void anIndexWrittenBeforeFactsMigratesWithoutLosingARow();
    void aVolumeRemembersWhatKindOfScanBuiltIt();
    void anAbandonedScanLeavesTheRecordedOptionsAlone();
    void anIndexWrittenBeforeTheOptionsAnswersNotKnown();
    void survivesAccessFromSeveralThreads();
    void aReadDoesNotWaitForAWriteInFlight();
    void operationsFailCleanlyWhenClosed();

private:
    IndexedFile makeFile(const QString& path, qint64 size = 0, bool isDir = false) const;
    /// A volume whose one finished scan holds `paths`.
    qint64 seedVolume(const QString& uri, const QStringList& paths);
    /// Writes rows into `volume` as one finished scan -- begun, filled and
    /// committed -- which is the only way anything becomes searchable.
    /// `options` is what the scan is recorded as having been asked for. Default
    /// for a test that does not care, stated for one that does.
    bool rescan(qint64 volume, const QList<IndexedFile>& rows, const ScanOptions& options = {});
    bool rescan(qint64 volume, const QStringList& paths);
    /// Rows in the table, whether or not a search can reach them.
    ///
    /// The whole arrangement rests on this differing from fileCount() while a
    /// scan is running, and a test that only ever asked fileCount() could not
    /// tell a row written and waiting from one never written at all.
    qint64 rowsInTable() const;
    /// The commonest query there is: one name to look for.
    static SearchQuery named(const QString& text)
    {
        SearchQuery query;
        query.add(SearchPredicate::name(text));
        return query;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<IndexDatabase> m_db;
};

void TestIndexDatabase::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_db = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite")));
    QVERIFY2(m_db->open().ok(), "index must open on a fresh path");
}

void TestIndexDatabase::cleanup()
{
    m_db.reset();
    m_dir.reset();
}

IndexedFile TestIndexDatabase::makeFile(const QString& path, qint64 size, bool isDir) const
{
    const VfsUri uri(QStringLiteral("file"), QString(), path);
    IndexedFile row;
    row.name = uri.fileName();
    row.path = uri.path();
    row.parentPath = uri.parent().path();
    row.extension = uri.suffix();
    row.isDir = isDir;
    row.size = size;
    row.modifiedEpoch = 1700000000;
    return row;
}

qint64 TestIndexDatabase::seedVolume(const QString& uri, const QStringList& paths)
{
    Result<qint64> volume = m_db->upsertVolume(VfsUri::fromString(uri), QStringLiteral("vol"));
    if (!volume.ok())
        return -1;
    return rescan(volume.value(), paths) ? volume.value() : -1;
}

bool TestIndexDatabase::rescan(qint64 volume, const QList<IndexedFile>& rows, const ScanOptions& options)
{
    const Result<qint64> scan = m_db->beginScan(volume);
    if (!scan.ok())
        return false;
    if (!m_db->insertBatch(volume, scan.value(), rows).ok())
        return false;
    return m_db->commitScan(volume, scan.value(), QDateTime::currentDateTime(), options).ok();
}

bool TestIndexDatabase::rescan(qint64 volume, const QStringList& paths)
{
    QList<IndexedFile> rows;
    rows.reserve(paths.size());
    for (const QString& path : paths)
        rows.append(makeFile(path));
    return rescan(volume, rows);
}

qint64 TestIndexDatabase::rowsInTable() const
{
    // Its own connection, deliberately: this asks the file what is in it
    // rather than asking the class under test what it will admit to.
    const QString name = QStringLiteral("raw_count");
    qint64 rows = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite")));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT COUNT(*) FROM files")) && query.next())
                rows = query.value(0).toLongLong();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return rows;
}

void TestIndexDatabase::opensAndCreatesSchema()
{
    QVERIFY(m_db->isOpen());
    Result<QList<IndexVolume>> volumes = m_db->volumes();
    QVERIFY2(volumes.ok(), qPrintable(volumes.error().message));
    QVERIFY(volumes.value().isEmpty());
}

void TestIndexDatabase::reopeningIsIdempotent()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite"));
    QVERIFY(seedVolume(QStringLiteral("file:///data"), { QStringLiteral("/data/a.txt") }) >= 0);
    m_db.reset();

    // Reopening an existing database must find the schema already applied and
    // the data intact -- this is the migration path every user will take.
    auto reopened = std::make_unique<IndexDatabase>(path);
    QVERIFY2(reopened->open().ok(), "reopening an existing index must succeed");
    Result<qint64> count = reopened->fileCount();
    QVERIFY(count.ok());
    QCOMPARE(count.value(), 1);
}

void TestIndexDatabase::upsertVolumeReturnsSameIdForSameRoot()
{
    const VfsUri root = VfsUri::fromString(QStringLiteral("sftp://nas/volume1"));
    Result<qint64> first = m_db->upsertVolume(root, QStringLiteral("NAS"));
    Result<qint64> second = m_db->upsertVolume(root, QStringLiteral("NAS again"));

    QVERIFY(first.ok());
    QVERIFY(second.ok());
    QCOMPARE(first.value(), second.value());
    QCOMPARE(m_db->volumes().value().size(), 1);
}

void TestIndexDatabase::insertBatchStoresRows()
{
    const qint64 volume = seedVolume(
        QStringLiteral("file:///data"), { QStringLiteral("/data/a.txt"), QStringLiteral("/data/b.txt") });
    QVERIFY(volume >= 0);

    QCOMPARE(m_db->fileCount(volume).value(), 2);
    // An empty batch is a legal no-op, not an error.
    QVERIFY(m_db->insertBatch(volume, 1, {}).ok());
    QCOMPARE(m_db->fileCount(volume).value(), 2);
}

void TestIndexDatabase::aWriteRefusedByAnotherConnectionSaysTheIndexWasLocked()
{
    // Every write failure in IndexDatabase arrives through one helper, and until
    // MOLE-306 it arrived as the driver's own text: "database is locked Unable to
    // fetch row" behind whatever the index was doing. A locked index is another
    // connection holding the file and waiting is the answer, which is a different
    // thing to be told than that the disk is full.
    //
    // Arranged with the equipment rather than by waiting: the index has the same
    // five-second busy timeout as the scratch store, and a suite that waits it out
    // once waits it out on every machine for ever. See tests/support/SqliteFaults.h.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite"));
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    QSqlDatabase writing = sqlite::connectionOn(path);
    QVERIFY2(writing.isValid(), "the index's connection has to be findable, or this case tests nothing");
    QVERIFY(sqlite::stopWaitingForLocks(writing));

    sqlite::WriteLock held(path);
    QVERIFY2(held.isHeld(), "the lock has to be taken, or the write below is unhindered");

    IndexedFile row;
    row.name = QStringLiteral("a.txt");
    row.path = QStringLiteral("/data/a.txt");
    row.parentPath = QStringLiteral("/data");
    row.extension = QStringLiteral("txt");
    const Result<void> written = m_db->insertBatch(volume.value(), 1, { row });

    QVERIFY2(!written.ok(), "a write into a locked index was reported as a write that landed");
    QVERIFY2(written.error().message.contains(QStringLiteral("locked by another connection")),
        qPrintable(written.error().message));
    QVERIFY2(!written.error().message.contains(QStringLiteral("Unable to fetch row")),
        qPrintable(written.error().message));
}

void TestIndexDatabase::aWriteThatRanOutOfRoomSaysWhatHappenedRatherThanNothing()
{
    // The fault found while writing the case above, and it is worse than the
    // wording it was about: every write failure in IndexDatabase reported the
    // connection's last error, and a *statement* that fails records its error on
    // itself. QSqlDatabase::lastError() is about opening, transactions and
    // commits, so for a failed INSERT it is empty -- and what a reader got was
    // "Inserting indexed file: " with nothing after the colon.
    //
    // Held with a failure that is not a locked database, so this case is about the
    // error arriving at all rather than about the sentence the other one asserts.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite"));
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    QSqlDatabase writing = sqlite::connectionOn(path);
    QVERIFY(writing.isValid());
    QVERIFY2(sqlite::capAtCurrentSize(writing), "the cap has to take, or the rows below are unhindered");

    QList<IndexedFile> rows;
    rows.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        IndexedFile row;
        row.name = QStringLiteral("file-%1-%2").arg(i).arg(QString(120, QLatin1Char('x')));
        row.path = QStringLiteral("/data/") + row.name;
        row.parentPath = QStringLiteral("/data");
        row.extension = QStringLiteral("txt");
        rows.append(row);
    }

    const Result<void> written = m_db->insertBatch(volume.value(), 1, rows);
    QVERIFY2(!written.ok(), "rows that did not fit were reported as rows that did");
    QVERIFY2(written.error().message.contains(QStringLiteral("disk is full")),
        qPrintable(written.error().message));
    // The shape of the old answer, spelled out so it cannot come back quietly.
    QVERIFY2(!written.error().message.endsWith(QStringLiteral(": ")), qPrintable(written.error().message));
}

void TestIndexDatabase::searchMatchesSubstring()
{
    QVERIFY(seedVolume(QStringLiteral("file:///data"),
                { QStringLiteral("/data/annual-report.pdf"), QStringLiteral("/data/notes.txt") })
        >= 0);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("report")));

    Result<QList<IndexSearchHit>> hits = m_db->search(query);
    QVERIFY2(hits.ok(), qPrintable(hits.error().message));
    QCOMPARE(hits.value().size(), 1);
    QCOMPARE(hits.value().first().name, QStringLiteral("annual-report.pdf"));
}

void TestIndexDatabase::searchIsCaseInsensitiveForNonAsciiByDefault()
{
    QVERIFY(seedVolume(QStringLiteral("file:///data"), { QStringLiteral("/data/ŁÓDŹ-raport.txt") }) >= 0);

    // SQLite's own NOCASE only folds ASCII, so this is the case that catches a
    // regression in the folded-name column.
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("łódź")));

    Result<QList<IndexSearchHit>> hits = m_db->search(query);
    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);
}

void TestIndexDatabase::searchHonoursCaseSensitiveFlag()
{
    QVERIFY(seedVolume(QStringLiteral("file:///data"),
                { QStringLiteral("/data/Report.txt"), QStringLiteral("/data/report.txt") })
        >= 0);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("Report"), true));

    Result<QList<IndexSearchHit>> hits = m_db->search(query);
    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);
    QCOMPARE(hits.value().first().name, QStringLiteral("Report.txt"));
}

void TestIndexDatabase::searchFiltersByExtensionAndKind()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    QVERIFY(rescan(volume.value(),
        { makeFile(QStringLiteral("/data/a.pdf")), makeFile(QStringLiteral("/data/a.txt")),
            makeFile(QStringLiteral("/data/subdir"), 0, true) }));

    SearchQuery byExtension; // matching must not be case sensitive
    byExtension.add(SearchPredicate::extensions({ QStringLiteral("PDF") }));
    QCOMPARE(m_db->search(byExtension).value().size(), 1);

    SearchQuery filesOnly;
    filesOnly.add(SearchPredicate::kind(false));
    QCOMPARE(m_db->search(filesOnly).value().size(), 2);

    SearchQuery dirsOnly;
    dirsOnly.add(SearchPredicate::kind(true));
    QCOMPARE(m_db->search(dirsOnly).value().size(), 1);

    // Asking for what is a directory and also a file. Two criteria that cannot
    // both hold answer with nothing, rather than with everything.
    SearchQuery neither;
    neither.add(SearchPredicate::kind(true));
    neither.add(SearchPredicate::kind(false));
    QVERIFY(m_db->search(neither).value().isEmpty());
}

void TestIndexDatabase::searchFiltersBySize()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    QVERIFY(rescan(volume.value(),
        { makeFile(QStringLiteral("/data/small.bin"), 100),
            makeFile(QStringLiteral("/data/big.bin"), 10'000) }));

    SearchQuery large;
    large.add(SearchPredicate::minSize(1000));
    QCOMPARE(m_db->search(large).value().size(), 1);
    QCOMPARE(m_db->search(large).value().first().name, QStringLiteral("big.bin"));

    SearchQuery small;
    small.add(SearchPredicate::maxSize(1000));
    QCOMPARE(m_db->search(small).value().size(), 1);
    QCOMPARE(m_db->search(small).value().first().name, QStringLiteral("small.bin"));
}

void TestIndexDatabase::searchScopesToVolume()
{
    const qint64 first = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/shared.txt") });
    const qint64 second = seedVolume(QStringLiteral("file:///two"), { QStringLiteral("/two/shared.txt") });
    QVERIFY(first >= 0 && second >= 0);

    SearchQuery all;
    all.add(SearchPredicate::name(QStringLiteral("shared")));
    QCOMPARE(m_db->search(all).value().size(), 2);

    SearchQuery scoped = all;
    scoped.volumeId = second;
    QCOMPARE(m_db->search(scoped).value().size(), 1);
    QCOMPARE(m_db->search(scoped).value().first().uri, QStringLiteral("file:///two/shared.txt"));
}

void TestIndexDatabase::searchRespectsLimit()
{
    QStringList paths;
    for (int i = 0; i < 50; ++i)
        paths.append(QStringLiteral("/data/file%1.txt").arg(i));
    QVERIFY(seedVolume(QStringLiteral("file:///data"), paths) >= 0);

    SearchQuery query;
    query.limit = 10;
    QCOMPARE(m_db->search(query).value().size(), 10);
}

void TestIndexDatabase::searchReturnsResolvableUris()
{
    QVERIFY(seedVolume(QStringLiteral("sftp://nas/volume1"), { QStringLiteral("/volume1/deep/a.txt") }) >= 0);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("a.txt")));
    Result<QList<IndexSearchHit>> hits = m_db->search(query);
    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);

    // A hit has to carry enough information to open the file on its backend,
    // scheme and authority included.
    const VfsUri uri = VfsUri::fromString(hits.value().first().uri);
    QCOMPARE(uri.scheme(), QStringLiteral("sftp"));
    QCOMPARE(uri.authority(), QStringLiteral("nas"));
    QCOMPARE(uri.path(), QStringLiteral("/volume1/deep/a.txt"));
}

/// What a search may see while a scan is running, which is the whole point of
/// there being a generation on the row at all.
void TestIndexDatabase::rowsFromAnUnfinishedScanAreNotVisible()
{
    const qint64 volume = seedVolume(QStringLiteral("file:///data"), { QStringLiteral("/data/settled.txt") });
    QVERIFY(volume >= 0);

    const Result<qint64> scan = m_db->beginScan(volume);
    QVERIFY2(scan.ok(), qPrintable(scan.error().message));
    QVERIFY(m_db->insertBatch(volume, scan.value(), { makeFile(QStringLiteral("/data/arriving.txt")) }).ok());

    // Written, and belonging to no volume yet: in the table, out of every answer.
    QCOMPARE(rowsInTable(), 2);
    QCOMPARE(m_db->fileCount(volume).value(), 1);

    QVERIFY2(m_db->search(named(QStringLiteral("arriving"))).value().isEmpty(),
        "a scan in progress answered a search");
    QCOMPARE(m_db->search(named(QStringLiteral("settled"))).value().size(), 1);

    const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
    QVERIFY(m_db->commitScan(volume, scan.value(), when, ScanOptions {}).ok());

    // And the swap is total in both directions: what arrived is the contents,
    // what it replaced is gone rather than lingering alongside it.
    QCOMPARE(rowsInTable(), 1);
    QCOMPARE(m_db->fileCount(volume).value(), 1);
    QCOMPARE(m_db->search(named(QStringLiteral("arriving"))).value().size(), 1);
    QVERIFY(m_db->search(named(QStringLiteral("settled"))).value().isEmpty());
}

void TestIndexDatabase::anAbandonedScanLeavesThePreviousContents()
{
    const qint64 volume = seedVolume(QStringLiteral("file:///data"), { QStringLiteral("/data/settled.txt") });
    QVERIFY(volume >= 0);
    const IndexVolume before = m_db->volumes().value().first();

    const Result<qint64> scan = m_db->beginScan(volume);
    QVERIFY(scan.ok());
    QVERIFY(m_db->insertBatch(volume, scan.value(), { makeFile(QStringLiteral("/data/arriving.txt")) }).ok());
    QVERIFY(m_db->abandonScan(volume, scan.value()).ok());

    QCOMPARE(rowsInTable(), 1);
    QCOMPARE(m_db->fileCount(volume).value(), 1);
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("settled")));
    QCOMPARE(m_db->search(query).value().size(), 1);

    // Nothing about the volume moved either: an abandoned scan is one that did
    // not happen, so the last one that did is still the last one.
    const IndexVolume after = m_db->volumes().value().first();
    QCOMPARE(after.fileCount, before.fileCount);
    QCOMPARE(after.lastScan, before.lastScan);
}

/// Handed the generation the volume is actually serving, abandoning must be a
/// no-op. It is a tidy-up, and a tidy-up able to empty a 4 TB index is exactly
/// the fault the generations were added to remove.
void TestIndexDatabase::abandoningTheVolumesOwnScanDoesNothing()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    const Result<qint64> scan = m_db->beginScan(volume.value());
    QVERIFY(scan.ok());
    QVERIFY(
        m_db->insertBatch(volume.value(), scan.value(), { makeFile(QStringLiteral("/data/a.txt")) }).ok());
    QVERIFY(
        m_db->commitScan(volume.value(), scan.value(), QDateTime::currentDateTime(), ScanOptions {}).ok());

    QVERIFY(m_db->abandonScan(volume.value(), scan.value()).ok());
    QCOMPARE(m_db->fileCount(volume.value()).value(), 1);
    QCOMPARE(rowsInTable(), 1);
}

/// A scan killed with the process commits nothing and abandons nothing: no
/// error path ran. Its rows are invisible for ever, so the next scan of the
/// volume sweeps them out -- otherwise the file grows by a dead scan each time.
void TestIndexDatabase::aScanNothingFinishedIsSweptUpByTheNext()
{
    const qint64 volume = seedVolume(QStringLiteral("file:///data"), { QStringLiteral("/data/settled.txt") });
    QVERIFY(volume >= 0);

    const Result<qint64> lost = m_db->beginScan(volume);
    QVERIFY(lost.ok());
    QVERIFY(m_db->insertBatch(volume, lost.value(),
                    { makeFile(QStringLiteral("/data/x.txt")), makeFile(QStringLiteral("/data/y.txt")) })
                .ok());
    QCOMPARE(rowsInTable(), 3);

    const Result<qint64> next = m_db->beginScan(volume);
    QVERIFY(next.ok());
    QVERIFY2(next.value() != lost.value(), "a second scan reused the generation of the first");
    QCOMPARE(rowsInTable(), 1); // the settled row, and nothing the lost scan wrote

    // And the contents it was going to replace are still the contents.
    QCOMPARE(m_db->fileCount(volume).value(), 1);
}

void TestIndexDatabase::aRescanTouchesOnlyItsOwnVolume()
{
    const qint64 first = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/a.txt") });
    const qint64 second = seedVolume(QStringLiteral("file:///two"), { QStringLiteral("/two/b.txt") });
    QVERIFY(first >= 0 && second >= 0);

    QVERIFY(rescan(first, QStringList { QStringLiteral("/one/c.txt") }));
    QCOMPARE(m_db->fileCount(first).value(), 1);
    QCOMPARE(m_db->fileCount(second).value(), 1);
    // The volume row itself survives so a rescan reuses the same id.
    QCOMPARE(m_db->volumes().value().size(), 2);

    QCOMPARE(m_db->search(named(QStringLiteral("b.txt"))).value().size(), 1);
    QVERIFY(m_db->search(named(QStringLiteral("a.txt"))).value().isEmpty());
}

void TestIndexDatabase::removeVolumeDropsEverything()
{
    const qint64 volume = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/a.txt") });
    QVERIFY(volume >= 0);

    QVERIFY(m_db->removeVolume(volume).ok());
    QVERIFY(m_db->volumes().value().isEmpty());
    QCOMPARE(m_db->fileCount().value(), 0);
}

void TestIndexDatabase::commitScanRecordsTheCountAndTheTime()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///one")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    const Result<qint64> scan = m_db->beginScan(volume.value());
    QVERIFY(scan.ok());
    QVERIFY(m_db->insertBatch(volume.value(), scan.value(),
                    { makeFile(QStringLiteral("/one/a.txt")), makeFile(QStringLiteral("/one/b.txt")) })
                .ok());

    // The volume says nothing has been scanned until the scan is committed --
    // a date on it is a claim that what it holds is that old, and while a scan
    // is running the claim would be for contents nobody can see yet.
    QVERIFY(!m_db->volumes().value().first().lastScan.isValid());
    QCOMPARE(m_db->volumes().value().first().fileCount, 0);

    const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
    QVERIFY(m_db->commitScan(volume.value(), scan.value(), when, ScanOptions {}).ok());

    const QList<IndexVolume> volumes = m_db->volumes().value();
    QCOMPARE(volumes.size(), 1);
    QCOMPARE(volumes.first().fileCount, 2);
    QCOMPARE(volumes.first().lastScan, when);
}

/// An index a user already has, opened by a version that knows about
/// generations. Both columns default to nought, which is what every row and
/// every volume in it already reads as -- so it stays visible in full. Getting
/// this wrong empties every index in the world on upgrade, silently.
void TestIndexDatabase::anIndexWrittenBeforeGenerationsStaysVisible()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("old.sqlite"));
    const QString name = QStringLiteral("schema_one");
    {
        // The schema exactly as version 1 shipped it, written by hand: the
        // migration cannot be tested against a database the migration built.
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE volumes (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                          "root_uri TEXT NOT NULL UNIQUE, label TEXT NOT NULL, "
                                          "last_scan INTEGER, file_count INTEGER NOT NULL DEFAULT 0)")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE files (id INTEGER PRIMARY KEY AUTOINCREMENT, volume_id INTEGER NOT NULL "
            "REFERENCES volumes(id) ON DELETE CASCADE, name TEXT NOT NULL, name_folded TEXT NOT NULL, "
            "path TEXT NOT NULL, parent_path TEXT NOT NULL, extension TEXT, "
            "is_dir INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0, "
            "mtime INTEGER NOT NULL DEFAULT 0)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO volumes (root_uri, label, last_scan, file_count) VALUES ('file:///old', 'old', "
            "1700000000, 2)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO files (volume_id, name, name_folded, path, parent_path, extension) VALUES "
            "(1, 'kept.txt', 'kept.txt', '/old/kept.txt', '/old', 'txt'), "
            "(1, 'also.txt', 'also.txt', '/old/also.txt', '/old', 'txt')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=1")));
    }
    QSqlDatabase::removeDatabase(name);

    IndexDatabase upgraded(path);
    QVERIFY2(upgraded.open().ok(), "an index from the previous schema must still open");

    QCOMPARE(upgraded.fileCount().value(), 2);
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("kept")));
    QCOMPARE(upgraded.search(query).value().size(), 1);
    QCOMPARE(upgraded.volumes().value().first().fileCount, 2);

    // And it can still be rescanned, which is where its rows finally get a
    // generation of their own.
    const qint64 volume = upgraded.volumes().value().first().id;
    const Result<qint64> scan = upgraded.beginScan(volume);
    QVERIFY(scan.ok());
    QVERIFY(upgraded.insertBatch(volume, scan.value(), { makeFile(QStringLiteral("/old/new.txt")) }).ok());
    QCOMPARE(upgraded.fileCount().value(), 2); // still the old two, mid-scan
    QVERIFY(upgraded.commitScan(volume, scan.value(), QDateTime::currentDateTime(), ScanOptions {}).ok());
    QCOMPARE(upgraded.fileCount().value(), 1);
}

/// Metadata is the one thing worth indexing precisely because the contents are
/// not: a camera and a date taken are a few dozen bytes where the photograph is
/// eight megabytes.
void TestIndexDatabase::whatAFileSaysAboutItselfIsStoredAndAskedFor()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///photos")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    IndexedFile canon = makeFile(QStringLiteral("/photos/a.jpg"));
    canon.facts = { SearchFact { QStringLiteral("image.camera"), QStringLiteral("Canon EOS 5D"), 0, false },
        SearchFact { QStringLiteral("image.iso"), QStringLiteral("ISO 400"), 400, true } };
    IndexedFile nikon = makeFile(QStringLiteral("/photos/b.jpg"));
    nikon.facts = { SearchFact { QStringLiteral("image.camera"), QStringLiteral("Nikon Z6"), 0, false },
        SearchFact { QStringLiteral("image.iso"), QStringLiteral("ISO 3200"), 3200, true } };
    IndexedFile plain = makeFile(QStringLiteral("/photos/notes.txt"));

    QVERIFY(rescan(volume.value(), { canon, nikon, plain }));

    // Text, folded, and only the file that said it.
    SearchQuery byCamera;
    byCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("canon")));
    QCOMPARE(m_db->search(byCamera).value().size(), 1);
    QCOMPARE(m_db->search(byCamera).value().first().name, QStringLiteral("a.jpg"));

    // A range, which is what the number column exists for.
    SearchQuery fast;
    fast.add(SearchPredicate::metadataAtLeast(QStringLiteral("image.iso"), 1000));
    QCOMPARE(m_db->search(fast).value().size(), 1);
    QCOMPARE(m_db->search(fast).value().first().name, QStringLiteral("b.jpg"));

    SearchQuery slow;
    slow.add(SearchPredicate::metadataAtMost(QStringLiteral("image.iso"), 1000));
    QCOMPARE(m_db->search(slow).value().size(), 1);
    QCOMPARE(m_db->search(slow).value().first().name, QStringLiteral("a.jpg"));

    // A file that says nothing is not a file that says everything.
    SearchQuery anyCamera;
    anyCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("o")));
    QCOMPARE(m_db->search(anyCamera).value().size(), 2);

    // And the index answers it rather than handing it back to be checked.
    QVERIFY(planSearch(byCamera, SearchSource::Index).pushedDownEverything());
    QVERIFY(planSearch(fast, SearchSource::Index).pushedDownEverything());

    // A rescan replaces the facts with the rows they belong to.
    QVERIFY(rescan(volume.value(), { plain }));
    QVERIFY(m_db->search(byCamera).value().isEmpty());
    QCOMPARE(rowsInTable(), 1);
}

/// A file inside an archive does not live on the volume's own scheme, and
/// rebuilding its uri from the volume's would put it loose on the disk.
void TestIndexDatabase::aRowInsideAContainerKeepsItsOwnAddress()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    IndexedFile loose = makeFile(QStringLiteral("/data/notes.txt"));
    IndexedFile member = makeFile(QStringLiteral("/inside.txt"));
    member.uri = QStringLiteral("archive://%2Fdata%2Fbackup.zip/inside.txt");

    QVERIFY(rescan(volume.value(), { loose, member }));

    SearchQuery all;
    all.add(SearchPredicate::name(QStringLiteral(".txt")));
    // Held in a local: Result hands back a reference into itself, so ranging
    // over the value of a temporary reads memory that has already gone.
    const Result<QList<IndexSearchHit>> found = m_db->search(all);
    QVERIFY(found.ok());
    QStringList uris;
    for (const IndexSearchHit& hit : found.value())
        uris.append(hit.uri);
    uris.sort();

    QCOMPARE(uris.size(), 2);
    QCOMPARE(uris.at(0), QStringLiteral("archive://%2Fdata%2Fbackup.zip/inside.txt"));
    QCOMPARE(uris.at(1), QStringLiteral("file:///data/notes.txt"));
}

/// A member is somewhere, and where it is is inside the folder holding its
/// container.
///
/// The `Under` criterion used to be answered nowhere near the index: it was
/// evaluated in memory against the row's uri, and a member's uri is on the
/// archive's own authority -- `archive://%2Fdata%2Fbackup.zip/inside.txt` is
/// under nothing at all as far as a string prefix is concerned. So searching a
/// folder could never return anything inside an archive in it, however
/// thoroughly the scan had recorded it. It is answered on the path column now,
/// which is the one column that says where a row really sits.
void TestIndexDatabase::aFolderScopedSearchFindsWhatIsInsideAnArchiveInThatFolder()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());

    IndexedFile here = makeFile(QStringLiteral("/data/reports/loose.txt"));
    IndexedFile member = makeFile(QStringLiteral("/data/reports/backup.zip!/inside.txt"));
    member.name = QStringLiteral("inside.txt");
    member.parentPath = QStringLiteral("/data/reports/backup.zip");
    member.uri = QStringLiteral("archive://%2Fdata%2Freports%2Fbackup.zip/inside.txt");
    IndexedFile elsewhere = makeFile(QStringLiteral("/data/other/loose.txt"));

    QVERIFY(rescan(volume.value(), { here, member, elsewhere }));

    const auto namesUnder = [this](const QString& folder) {
        SearchQuery query;
        query.add(SearchPredicate::underPath(folder));
        const Result<QList<IndexSearchHit>> found = m_db->search(query);
        QStringList names;
        if (!found.ok())
            return names;
        for (const IndexSearchHit& hit : found.value())
            names.append(hit.name);
        names.sort();
        return names;
    };

    QCOMPARE(namesUnder(QStringLiteral("file:///data/reports")),
        QStringList({ QStringLiteral("inside.txt"), QStringLiteral("loose.txt") }));
    // And the scope still means something: the other folder's file is not in it.
    QCOMPARE(namesUnder(QStringLiteral("file:///data/other")), QStringList { QStringLiteral("loose.txt") });
    // The archive itself is a folder to look in as much as any other name is.
    QCOMPARE(namesUnder(QStringLiteral("file:///data/reports/backup.zip")),
        QStringList { QStringLiteral("inside.txt") });
}

/// A carry that could not be written has to say so, because the caller acts on
/// the answer.
///
/// ScanTask reads this result to decide whether the subtree it just skipped is
/// safely in the new generation. A failure reported as a count of zero would be
/// indistinguishable from an empty folder -- and the scan would go on to commit
/// a generation the subtree is missing from.
void TestIndexDatabase::carryingASubtreeForwardIntoALockedIndexSaysSoRatherThanCarryingNothing()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite"));
    const qint64 volume = seedVolume(QStringLiteral("file:///data"),
        { QStringLiteral("/data/steady"), QStringLiteral("/data/steady/kept.txt") });
    QVERIFY(volume >= 0);

    QSqlDatabase writing = sqlite::connectionOn(path);
    QVERIFY2(writing.isValid(), "the index's connection has to be findable, or this case tests nothing");
    QVERIFY(sqlite::stopWaitingForLocks(writing));

    const Result<qint64> generation = m_db->beginScan(volume);
    QVERIFY(generation.ok());

    sqlite::WriteLock held(path);
    QVERIFY2(held.isHeld(), "the lock has to be taken, or the carry below is unhindered");

    const Result<qint64> carried
        = m_db->carryForward(volume, generation.value(), QStringLiteral("/data/steady"));

    QVERIFY2(!carried.ok(), "a carry into a locked index was reported as a carry of nothing");
}

/// The migration every user takes, from the schema the generations arrived in.
void TestIndexDatabase::anIndexWrittenBeforeFactsMigratesWithoutLosingARow()
{
    const qint64 volume = seedVolume(
        QStringLiteral("file:///data"), { QStringLiteral("/data/a.txt"), QStringLiteral("/data/b.txt") });
    QVERIFY(volume >= 0);
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite"));
    m_db.reset();

    // Wound back to the schema before this one, the way a database written by
    // the previous version really is.
    const QString name = QStringLiteral("wound_back");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("DROP TABLE IF EXISTS file_facts")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=2")));
        // Nor did it record what a scan was asked for. A column left behind here
        // would make the migration that adds it fail, which is not what a
        // database written by the previous version looks like.
        for (const QString& column : { QStringLiteral("scan_incremental"), QStringLiteral("scan_metadata"),
                 QStringLiteral("scan_archives") }) {
            QVERIFY(query.exec(QStringLiteral("ALTER TABLE volumes DROP COLUMN %1").arg(column)));
        }
        // Version 2 had no `uri` column either; SQLite cannot drop one, so the
        // table is rebuilt the way the previous schema really had it.
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE files_v2 AS SELECT id, volume_id, generation, "
                                          "name, name_folded, path, parent_path, extension, is_dir, "
                                          "size, mtime FROM files")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE files")));
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE files_v2 RENAME TO files")));
    }
    QSqlDatabase::removeDatabase(name);

    auto upgraded = std::make_unique<IndexDatabase>(path);
    QVERIFY2(upgraded->open().ok(), "an index from the previous schema must still open");
    QCOMPARE(upgraded->fileCount().value(), 2);
    QCOMPARE(upgraded->search(named(QStringLiteral("a.txt"))).value().size(), 1);

    // And opening it a second time changes nothing.
    upgraded.reset();
    auto again = std::make_unique<IndexDatabase>(path);
    QVERIFY(again->open().ok());
    QCOMPARE(again->fileCount().value(), 2);
}

/// Two indexes over two trees, one of which can answer a question about a camera
/// and one of which cannot, are different things and used to be
/// indistinguishable -- factKeys() is a proxy and cannot tell "scanned without
/// metadata" from "scanned with it over files that carry none". See MOLE-229.
void TestIndexDatabase::aVolumeRemembersWhatKindOfScanBuiltIt()
{
    const Result<qint64> photos
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///photos")), QStringLiteral("photos"));
    const Result<qint64> plain
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///plain")), QStringLiteral("plain"));
    QVERIFY(photos.ok());
    QVERIFY(plain.ok());

    ScanOptions everything;
    everything.incremental = true;
    everything.metadata = true;
    everything.archives = true;
    QVERIFY(rescan(photos.value(), { makeFile(QStringLiteral("/photos/a.jpg")) }, everything));
    QVERIFY(rescan(plain.value(), { makeFile(QStringLiteral("/plain/a.txt")) }, ScanOptions {}));

    const QList<IndexVolume> volumes = m_db->volumes().value();
    QCOMPARE(volumes.size(), 2);
    const auto found = [&volumes](const QString& label) {
        for (const IndexVolume& volume : volumes) {
            if (volume.label == label)
                return volume;
        }
        return IndexVolume {};
    };

    const IndexVolume rich = found(QStringLiteral("photos"));
    QVERIFY(rich.scan.has_value());
    QVERIFY(rich.scan->metadata);
    QVERIFY(rich.scan->archives);
    QVERIFY(rich.scan->incremental);

    const IndexVolume bare = found(QStringLiteral("plain"));
    QVERIFY(bare.scan.has_value());
    QVERIFY(!bare.scan->metadata);
    QVERIFY(!bare.scan->archives);
    QVERIFY(!bare.scan->incremental);
}

/// A scan that is abandoned or killed leaves the volume exactly as it was, and
/// what it was asked for is part of "as it was" -- the same property the
/// generation swap exists for, which should not gain an exception.
void TestIndexDatabase::anAbandonedScanLeavesTheRecordedOptionsAlone()
{
    const Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///tree")), QStringLiteral("tree"));
    QVERIFY(volume.ok());

    ScanOptions withMetadata;
    withMetadata.metadata = true;
    QVERIFY(rescan(volume.value(), { makeFile(QStringLiteral("/tree/a.jpg")) }, withMetadata));
    const QDateTime committed = m_db->volumes().value().first().lastScan;
    QVERIFY(committed.isValid());

    // A poorer scan, begun and thrown away.
    const Result<qint64> abandoned = m_db->beginScan(volume.value());
    QVERIFY(abandoned.ok());
    QVERIFY(m_db->insertBatch(volume.value(), abandoned.value(), { makeFile(QStringLiteral("/tree/b.jpg")) })
                .ok());
    QVERIFY(m_db->abandonScan(volume.value(), abandoned.value()).ok());

    const IndexVolume after = m_db->volumes().value().first();
    QCOMPARE(after.lastScan, committed);
    QVERIFY(after.scan.has_value());
    QVERIFY2(after.scan->metadata, "an abandoned scan must not rewrite what the volume was asked for");
}

/// An index somebody already has says "not known" rather than "no metadata",
/// because the second is a lie about a tree indexed with it last week. One
/// rescan replaces the unknown with the truth.
void TestIndexDatabase::anIndexWrittenBeforeTheOptionsAnswersNotKnown()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("no-options.sqlite"));
    const QString name = QStringLiteral("schema_four");
    {
        // The schema as version 4 shipped it, written by hand: the migration
        // cannot be tested against a database the migration built.
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE volumes (id INTEGER PRIMARY KEY AUTOINCREMENT, root_uri TEXT NOT NULL UNIQUE, "
            "label TEXT NOT NULL, last_scan INTEGER, file_count INTEGER NOT NULL DEFAULT 0, "
            "generation INTEGER NOT NULL DEFAULT 0, next_generation INTEGER NOT NULL DEFAULT 0)")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE files (id INTEGER PRIMARY KEY AUTOINCREMENT, volume_id INTEGER NOT NULL "
            "REFERENCES volumes(id) ON DELETE CASCADE, name TEXT NOT NULL, name_folded TEXT NOT NULL, "
            "path TEXT NOT NULL, parent_path TEXT NOT NULL, extension TEXT, "
            "is_dir INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0, "
            "mtime INTEGER NOT NULL DEFAULT 0, generation INTEGER NOT NULL DEFAULT 0, uri TEXT)")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE file_facts (file_id INTEGER NOT NULL "
                                          "REFERENCES files(id) ON DELETE CASCADE, key TEXT NOT NULL, "
                                          "text TEXT, num REAL)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO volumes (root_uri, label, last_scan, file_count) VALUES ('file:///old', 'old', "
            "1700000000, 1)")));
        QVERIFY(query.exec(
            QStringLiteral("INSERT INTO files (volume_id, name, name_folded, path, parent_path, extension) "
                           "VALUES (1, 'kept.jpg', 'kept.jpg', '/old/kept.jpg', '/old', 'jpg')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=4")));
    }
    QSqlDatabase::removeDatabase(name);

    IndexDatabase upgraded(path);
    QVERIFY2(upgraded.open().ok(), "an index from the previous schema must still open");

    // The migration loses no volume and no row.
    const QList<IndexVolume> volumes = upgraded.volumes().value();
    QCOMPARE(volumes.size(), 1);
    QCOMPARE(volumes.first().fileCount, 1);
    QCOMPARE(upgraded.fileCount().value(), 1);

    // And says it cannot tell, which is neither of the two answers a scanned
    // volume gives.
    QVERIFY2(!volumes.first().scan.has_value(),
        "a volume written before the options were recorded cannot claim to have none");

    // One rescan replaces the unknown with the truth.
    const qint64 volume = volumes.first().id;
    const Result<qint64> scan = upgraded.beginScan(volume);
    QVERIFY(scan.ok());
    QVERIFY(upgraded.insertBatch(volume, scan.value(), { makeFile(QStringLiteral("/old/kept.jpg")) }).ok());
    ScanOptions withMetadata;
    withMetadata.metadata = true;
    QVERIFY(upgraded.commitScan(volume, scan.value(), QDateTime::currentDateTime(), withMetadata).ok());

    const IndexVolume rescanned = upgraded.volumes().value().first();
    QVERIFY(rescanned.scan.has_value());
    QVERIFY(rescanned.scan->metadata);
}

void TestIndexDatabase::survivesAccessFromSeveralThreads()
{
    // A QSqlDatabase connection belongs to the thread that opened it. Scans
    // run on pool threads while the UI searches from the main one, so the
    // index has to hand out a connection per thread -- this is the regression
    // test for that, and it fails loudly (not subtly) if it ever regresses.
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    const Result<qint64> scan = m_db->beginScan(volume.value());
    QVERIFY(scan.ok());

    constexpr int kWriters = 4;
    constexpr int kRowsEach = 50;

    std::atomic_int failures { 0 };
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([this, &failures, volumeId = volume.value(), generation = scan.value(), w] {
            QList<IndexedFile> rows;
            for (int i = 0; i < kRowsEach; ++i)
                rows.append(makeFile(QStringLiteral("/data/w%1-f%2.txt").arg(w).arg(i)));
            if (!m_db->insertBatch(volumeId, generation, rows).ok())
                ++failures;
        });
    }
    for (std::thread& writer : writers)
        writer.join();

    QCOMPARE(failures.load(), 0);
    QVERIFY(
        m_db->commitScan(volume.value(), scan.value(), QDateTime::currentDateTime(), ScanOptions {}).ok());
    QCOMPARE(m_db->fileCount().value(), kWriters * kRowsEach);

    // Reading from yet another thread must work too.
    std::atomic_int seen { -1 };
    std::thread reader([this, &seen] {
        SearchQuery query;
        query.add(SearchPredicate::name(QStringLiteral("w0-")));
        Result<QList<IndexSearchHit>> hits = m_db->search(query);
        seen = hits.ok() ? hits.value().size() : -1;
    });
    reader.join();

    QCOMPARE(seen.load(), kRowsEach);
}

void TestIndexDatabase::aReadDoesNotWaitForAWriteInFlight()
{
    // The index opens its database in WAL mode so that a scan can write while
    // the interface reads, and then used to put both behind one mutex, which
    // handed that concurrency straight back. Measured on an index of 723,405
    // files, the thread that draws the window sat at 0% CPU for sixty seconds
    // while the scan ran at 99.9% and the window never appeared. See ADR-0065.
    //
    // The claim is a condition and not a clock: **the read returns while the
    // write is still in flight**. Both directions of that rest on orderings with
    // margins of a thousandfold, not on a scheduler being fair:
    //
    //  - the writer holds the lock before the reader asks for it, because it
    //    acquires nanoseconds after setting writeStarted while the reader is
    //    still being spawned, which is microseconds;
    //  - under one lock the read cannot return until the write ends, and by then
    //    writeDone is set -- the writer sets it nanoseconds after releasing,
    //    while the woken reader still has a select to run;
    //  - under two, the read returns inside a transaction that has hundreds of
    //    milliseconds left.
    //
    // Two earlier shapes of this test were thrown away for asserting nothing.
    // One built its rows inside the writer's loop, which left a gap on every
    // iteration and passed 20 times out of 20 against the fault. The other read
    // in a tight loop, which barged the writer out of its own lock: it was the
    // *writer* that starved, writeDone never came, and every read counted.
    constexpr int kRows = 50000;

    const Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    const Result<qint64> scan = m_db->beginScan(volume.value());
    QVERIFY(scan.ok());

    // Built by hand rather than through makeFile(): fifty thousand VfsUri
    // parses would be the test's runtime rather than its subject.
    QList<IndexedFile> rows;
    rows.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        IndexedFile row;
        row.name = QStringLiteral("f%1.txt").arg(i);
        row.parentPath = QStringLiteral("/data");
        row.path = row.parentPath + QLatin1Char('/') + row.name;
        row.extension = QStringLiteral("txt");
        row.size = i;
        row.modifiedEpoch = 1700000000;
        rows.append(std::move(row));
    }

    std::atomic_bool writeStarted { false };
    std::atomic_bool writeDone { false };
    std::atomic_bool writeOk { false };

    std::thread writer([this, &writeStarted, &writeDone, &writeOk, &rows, volumeId = volume.value(),
                           generation = scan.value()] {
        writeStarted = true;
        writeOk = m_db->insertBatch(volumeId, generation, rows).ok();
        writeDone = true;
    });

    // Wait for the writer to be underway, not for a length of time.
    while (!writeStarted.load())
        std::this_thread::yield();

    std::atomic_bool readOk { false };
    std::atomic_bool writerStillWorking { false };
    std::thread reader([this, &readOk, &writeDone, &writerStillWorking] {
        // volumes() is the read the window makes on startup: a select over a
        // handful of rows, which is why sixty seconds of it was never the
        // query's fault. Its own connection is opened here, under no lock of
        // ours, which is itself part of what changed.
        const bool ok = m_db->volumes().ok();
        // Sampled the instant the read returns, before anything else can move.
        writerStillWorking = !writeDone.load();
        readOk = ok;
    });

    reader.join();
    writer.join();

    QVERIFY2(writeOk.load(), "the writer's batch must have gone in");
    QVERIFY2(readOk.load(), "the read must have succeeded");
    QVERIFY2(writerStillWorking.load(),
        "the read did not return until the write had finished -- it is queueing behind the writer "
        "instead of taking its own snapshot");

    // And the write was real, so the read was not racing an idle thread.
    QCOMPARE(rowsInTable(), qint64(kRows));
}

void TestIndexDatabase::operationsFailCleanlyWhenClosed()
{
    m_db->close();
    QVERIFY(!m_db->isOpen());

    // Every entry point must report a plain error instead of crashing.
    QVERIFY(!m_db->volumes().ok());
    QVERIFY(!m_db->fileCount().ok());
    QVERIFY(!m_db->search(SearchQuery {}).ok());
    QVERIFY(!m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///x")), QString()).ok());
    QVERIFY(!m_db->beginScan(1).ok());
    QVERIFY(!m_db->insertBatch(1, 1, { makeFile(QStringLiteral("/x/a")) }).ok());
    QVERIFY(!m_db->commitScan(1, 1, QDateTime::currentDateTime(), ScanOptions {}).ok());
    QVERIFY(!m_db->abandonScan(1, 1).ok());
}

MOLE_TEST_MAIN(TestIndexDatabase)
#include "tst_IndexDatabase.moc"
