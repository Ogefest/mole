#include "support/MoleTestMain.h"
#include "support/SqliteFaults.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace mole::test;

/// The equipment, held to what it claims -- the same way the other fakes in this
/// directory are.
///
/// A fault injector nobody checks is worse than none: a case that arranges a
/// failure which does not happen asserts on whatever the code did instead, and
/// reads as evidence. See MOLE-293.
class TestSqliteFaults : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aHeldWriteLockRefusesAnotherConnectionsWrite();
    void andItCostsNoTimeAtAll();
    void aCapMakesTheNextWriteSayTheDiskIsFull();
    void theLockAndTheCapAreBothUndoneAfterwards();
    void aStaleReadSnapshotIsRefusedWithAnExtendedCode();
    void anExtendedCodeIsStillReadAsWhatItIs();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    static constexpr auto kName = "sqlite-faults-under-test";

    /// A connection shaped like a store's: WAL, and a busy timeout it would
    /// otherwise wait out.
    QSqlDatabase openLikeAStore()
    {
        QSqlDatabase database
            = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QString::fromLatin1(kName));
        database.setDatabaseName(m_path);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!database.open())
            return {};
        QSqlQuery query(database);
        query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        query.exec(QStringLiteral("CREATE TABLE rows_ (c0 TEXT)"));
        return database;
    }
};

void TestSqliteFaults::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = QDir(m_dir->path()).filePath(QStringLiteral("store.sqlite"));
}

void TestSqliteFaults::cleanup()
{
    {
        QSqlDatabase database = QSqlDatabase::database(QString::fromLatin1(kName), false);
        if (database.isValid())
            database.close();
    }
    QSqlDatabase::removeDatabase(QString::fromLatin1(kName));
    m_dir.reset();
}

void TestSqliteFaults::aHeldWriteLockRefusesAnotherConnectionsWrite()
{
    QSqlDatabase store = openLikeAStore();
    QVERIFY(store.isOpen());

    // Found the way a case finds a real store's connection, so this is the seam
    // being checked as well as the lock.
    QSqlDatabase found = sqlite::connectionOn(m_path);
    QVERIFY2(found.isValid(), "the connection on this file has to be findable");
    QVERIFY(sqlite::stopWaitingForLocks(found));

    sqlite::WriteLock lock(m_path);
    QVERIFY2(lock.isHeld(), "the lock has to be taken, or nothing below is being stopped");

    QSqlQuery write(store);
    QVERIFY2(!write.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('x')")),
        "a write with the lock held elsewhere has to be refused");
    QCOMPARE(sqlite::primaryCode(write.lastError().nativeErrorCode()), sqlite::kBusy);
}

void TestSqliteFaults::andItCostsNoTimeAtAll()
{
    // The whole reason this exists rather than a test that holds a lock and waits:
    // the store's timeout is five seconds, and a suite that sleeps for it once is
    // a suite that sleeps for it every run, on every machine.
    QSqlDatabase store = openLikeAStore();
    QVERIFY(store.isOpen());
    QSqlDatabase found = sqlite::connectionOn(m_path);
    QVERIFY(sqlite::stopWaitingForLocks(found));
    sqlite::WriteLock lock(m_path);
    QVERIFY(lock.isHeld());

    QElapsedTimer timer;
    timer.start();
    QSqlQuery write(store);
    QVERIFY(!write.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('x')")));
    // Generous by three orders of magnitude against the thing it is ruling out,
    // which is a five-second wait: what is asserted is that nothing waited, not
    // how fast this machine is.
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
}

void TestSqliteFaults::aCapMakesTheNextWriteSayTheDiskIsFull()
{
    QSqlDatabase store = openLikeAStore();
    QVERIFY(store.isOpen());
    QSqlDatabase found = sqlite::connectionOn(m_path);
    QVERIFY2(sqlite::capAt(found, 3), "the cap has to take, or the writes below are unhindered");

    QSqlQuery insert(store);
    QVERIFY(insert.prepare(QStringLiteral("INSERT INTO rows_ (c0) VALUES (?)")));
    bool refused = false;
    for (int i = 0; i < 5000 && !refused; ++i) {
        insert.addBindValue(QString(400, QLatin1Char('x')));
        if (!insert.exec()) {
            refused = true;
            QCOMPARE(sqlite::primaryCode(insert.lastError().nativeErrorCode()), sqlite::kFull);
            QVERIFY2(insert.lastError().text().contains(QStringLiteral("disk is full")),
                qPrintable(insert.lastError().text()));
        }
    }
    QVERIFY2(refused, "five thousand rows into a three-page database were all accepted");
}

void TestSqliteFaults::theLockAndTheCapAreBothUndoneAfterwards()
{
    // Equipment that leaves a database unusable would make the next case in a
    // suite fail for a reason belonging to this one.
    QSqlDatabase store = openLikeAStore();
    QVERIFY(store.isOpen());
    QSqlDatabase found = sqlite::connectionOn(m_path);
    QVERIFY(sqlite::stopWaitingForLocks(found));

    {
        sqlite::WriteLock lock(m_path);
        QVERIFY(lock.isHeld());
        QSqlQuery blocked(store);
        QVERIFY(!blocked.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('x')")));
    }

    QSqlQuery afterwards(store);
    QVERIFY2(afterwards.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('y')")),
        qPrintable(afterwards.lastError().text()));

    // And the cap lifts, which is a pragma rather than anything written into the
    // file -- so it is worth being sure the number goes back up.
    QVERIFY(sqlite::capAt(found, 3));
    QSqlQuery lift(found);
    QVERIFY(lift.exec(QStringLiteral("PRAGMA max_page_count=1000000")));
    QSqlQuery more(store);
    QVERIFY2(more.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('z')")),
        qPrintable(more.lastError().text()));
}

void TestSqliteFaults::aStaleReadSnapshotIsRefusedWithAnExtendedCode()
{
    // The third arrangement, and the one that produces an *extended* code: a
    // connection reads, another writes, and the first one's write is refused at
    // once -- without the busy handler being consulted at all, because there is
    // nothing to wait for. 517 is SQLITE_BUSY_SNAPSHOT, and a comparison against
    // "5" does not see it. See MOLE-306.
    QSqlDatabase store = openLikeAStore();
    QVERIFY(store.isOpen());
    QSqlDatabase found = sqlite::connectionOn(m_path);
    QVERIFY(found.isValid());

    QElapsedTimer timer;
    timer.start();
    sqlite::StaleReadSnapshot stale(found, m_path);
    QVERIFY2(stale.isStale(), "the snapshot has to go stale, or the write below is unhindered");

    QSqlQuery write(store);
    QVERIFY2(!write.exec(QStringLiteral("INSERT INTO rows_ (c0) VALUES ('too late')")),
        "a write from a stale snapshot has to be refused");
    QCOMPARE(write.lastError().nativeErrorCode(), QStringLiteral("517"));
    QCOMPARE(sqlite::primaryCode(write.lastError().nativeErrorCode()), sqlite::kBusy);
    // Nothing waited: this is not the busy timeout expiring, which is the whole
    // reason it needs a sentence of its own.
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
}

void TestSqliteFaults::anExtendedCodeIsStillReadAsWhatItIs()
{
    // Qt reports SQLite's extended code where there is one, and 517 is
    // SQLITE_BUSY_SNAPSHOT -- a busy database that a comparison against "5" does
    // not recognise. primaryCode() is what makes a case able to say "this was
    // busy" whichever of the two arrived.
    QCOMPARE(sqlite::primaryCode(QStringLiteral("5")), sqlite::kBusy);
    QCOMPARE(sqlite::primaryCode(QStringLiteral("517")), sqlite::kBusy);
    QCOMPARE(sqlite::primaryCode(QStringLiteral("261")), sqlite::kBusy);
    QCOMPARE(sqlite::primaryCode(QStringLiteral("262")), sqlite::kLocked);
    QCOMPARE(sqlite::primaryCode(QStringLiteral("13")), sqlite::kFull);
    QCOMPARE(sqlite::primaryCode(QString()), -1);
    QCOMPARE(sqlite::primaryCode(QStringLiteral("not a number")), -1);
}

MOLE_TEST_MAIN(TestSqliteFaults)

#include "tst_SqliteFaults.moc"
