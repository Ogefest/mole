#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/index/IndexDatabase.h"

#include <QDir>

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
    void searchMatchesSubstring();
    void searchIsCaseInsensitiveForNonAsciiByDefault();
    void searchHonoursCaseSensitiveFlag();
    void searchFiltersByExtensionAndKind();
    void searchFiltersBySize();
    void searchScopesToVolume();
    void searchRespectsLimit();
    void searchReturnsResolvableUris();
    void clearVolumeDropsOnlyItsRows();
    void removeVolumeDropsEverything();
    void markVolumeScannedRecordsCount();
    void survivesAccessFromSeveralThreads();
    void operationsFailCleanlyWhenClosed();

private:
    IndexedFile makeFile(const QString& path, qint64 size = 0, bool isDir = false) const;
    qint64 seedVolume(const QString& uri, const QStringList& paths);

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

    QList<IndexedFile> rows;
    for (const QString& path : paths)
        rows.append(makeFile(path));
    return m_db->insertBatch(volume.value(), rows).ok() ? volume.value() : -1;
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
    QVERIFY(m_db->insertBatch(volume, {}).ok());
    QCOMPARE(m_db->fileCount(volume).value(), 2);
}

void TestIndexDatabase::searchMatchesSubstring()
{
    QVERIFY(seedVolume(QStringLiteral("file:///data"),
                { QStringLiteral("/data/annual-report.pdf"), QStringLiteral("/data/notes.txt") })
        >= 0);

    IndexSearchQuery query;
    query.text = QStringLiteral("report");

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
    IndexSearchQuery query;
    query.text = QStringLiteral("łódź");

    Result<QList<IndexSearchHit>> hits = m_db->search(query);
    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);
}

void TestIndexDatabase::searchHonoursCaseSensitiveFlag()
{
    QVERIFY(seedVolume(QStringLiteral("file:///data"),
                { QStringLiteral("/data/Report.txt"), QStringLiteral("/data/report.txt") })
        >= 0);

    IndexSearchQuery query;
    query.text = QStringLiteral("Report");
    query.caseSensitive = true;

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
    QVERIFY(m_db->insertBatch(volume.value(),
                    { makeFile(QStringLiteral("/data/a.pdf")), makeFile(QStringLiteral("/data/a.txt")),
                        makeFile(QStringLiteral("/data/subdir"), 0, true) })
                .ok());

    IndexSearchQuery byExtension;
    byExtension.extension = QStringLiteral("PDF"); // matching must not be case sensitive
    QCOMPARE(m_db->search(byExtension).value().size(), 1);

    IndexSearchQuery filesOnly;
    filesOnly.includeDirs = false;
    QCOMPARE(m_db->search(filesOnly).value().size(), 2);

    IndexSearchQuery dirsOnly;
    dirsOnly.includeFiles = false;
    QCOMPARE(m_db->search(dirsOnly).value().size(), 1);

    IndexSearchQuery neither;
    neither.includeFiles = false;
    neither.includeDirs = false;
    QVERIFY(m_db->search(neither).value().isEmpty());
}

void TestIndexDatabase::searchFiltersBySize()
{
    Result<qint64> volume
        = m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///data")), QStringLiteral("vol"));
    QVERIFY(volume.ok());
    QVERIFY(m_db->insertBatch(volume.value(),
                    { makeFile(QStringLiteral("/data/small.bin"), 100),
                        makeFile(QStringLiteral("/data/big.bin"), 10'000) })
                .ok());

    IndexSearchQuery large;
    large.minSize = 1000;
    QCOMPARE(m_db->search(large).value().size(), 1);
    QCOMPARE(m_db->search(large).value().first().name, QStringLiteral("big.bin"));

    IndexSearchQuery small;
    small.maxSize = 1000;
    QCOMPARE(m_db->search(small).value().size(), 1);
    QCOMPARE(m_db->search(small).value().first().name, QStringLiteral("small.bin"));
}

void TestIndexDatabase::searchScopesToVolume()
{
    const qint64 first = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/shared.txt") });
    const qint64 second = seedVolume(QStringLiteral("file:///two"), { QStringLiteral("/two/shared.txt") });
    QVERIFY(first >= 0 && second >= 0);

    IndexSearchQuery all;
    all.text = QStringLiteral("shared");
    QCOMPARE(m_db->search(all).value().size(), 2);

    IndexSearchQuery scoped = all;
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

    IndexSearchQuery query;
    query.limit = 10;
    QCOMPARE(m_db->search(query).value().size(), 10);
}

void TestIndexDatabase::searchReturnsResolvableUris()
{
    QVERIFY(seedVolume(QStringLiteral("sftp://nas/volume1"), { QStringLiteral("/volume1/deep/a.txt") }) >= 0);

    IndexSearchQuery query;
    query.text = QStringLiteral("a.txt");
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

void TestIndexDatabase::clearVolumeDropsOnlyItsRows()
{
    const qint64 first = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/a.txt") });
    const qint64 second = seedVolume(QStringLiteral("file:///two"), { QStringLiteral("/two/b.txt") });
    QVERIFY(first >= 0 && second >= 0);

    QVERIFY(m_db->clearVolume(first).ok());
    QCOMPARE(m_db->fileCount(first).value(), 0);
    QCOMPARE(m_db->fileCount(second).value(), 1);
    // The volume row itself survives so a rescan reuses the same id.
    QCOMPARE(m_db->volumes().value().size(), 2);
}

void TestIndexDatabase::removeVolumeDropsEverything()
{
    const qint64 volume = seedVolume(QStringLiteral("file:///one"), { QStringLiteral("/one/a.txt") });
    QVERIFY(volume >= 0);

    QVERIFY(m_db->removeVolume(volume).ok());
    QVERIFY(m_db->volumes().value().isEmpty());
    QCOMPARE(m_db->fileCount().value(), 0);
}

void TestIndexDatabase::markVolumeScannedRecordsCount()
{
    const qint64 volume = seedVolume(
        QStringLiteral("file:///one"), { QStringLiteral("/one/a.txt"), QStringLiteral("/one/b.txt") });
    QVERIFY(volume >= 0);

    const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
    QVERIFY(m_db->markVolumeScanned(volume, when).ok());

    const QList<IndexVolume> volumes = m_db->volumes().value();
    QCOMPARE(volumes.size(), 1);
    QCOMPARE(volumes.first().fileCount, 2);
    QCOMPARE(volumes.first().lastScan, when);
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

    constexpr int kWriters = 4;
    constexpr int kRowsEach = 50;

    std::atomic_int failures { 0 };
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([this, &failures, volumeId = volume.value(), w] {
            QList<IndexedFile> rows;
            for (int i = 0; i < kRowsEach; ++i)
                rows.append(makeFile(QStringLiteral("/data/w%1-f%2.txt").arg(w).arg(i)));
            if (!m_db->insertBatch(volumeId, rows).ok())
                ++failures;
        });
    }
    for (std::thread& writer : writers)
        writer.join();

    QCOMPARE(failures.load(), 0);
    QCOMPARE(m_db->fileCount().value(), kWriters * kRowsEach);

    // Reading from yet another thread must work too.
    std::atomic_int seen { -1 };
    std::thread reader([this, &seen] {
        IndexSearchQuery query;
        query.text = QStringLiteral("w0-");
        Result<QList<IndexSearchHit>> hits = m_db->search(query);
        seen = hits.ok() ? hits.value().size() : -1;
    });
    reader.join();

    QCOMPARE(seen.load(), kRowsEach);
}

void TestIndexDatabase::operationsFailCleanlyWhenClosed()
{
    m_db->close();
    QVERIFY(!m_db->isOpen());

    // Every entry point must report a plain error instead of crashing.
    QVERIFY(!m_db->volumes().ok());
    QVERIFY(!m_db->fileCount().ok());
    QVERIFY(!m_db->search(IndexSearchQuery {}).ok());
    QVERIFY(!m_db->upsertVolume(VfsUri::fromString(QStringLiteral("file:///x")), QString()).ok());
    QVERIFY(!m_db->insertBatch(1, { makeFile(QStringLiteral("/x/a")) }).ok());
    QVERIFY(!m_db->clearVolume(1).ok());
}

MOLE_TEST_MAIN(TestIndexDatabase)
#include "tst_IndexDatabase.moc"
