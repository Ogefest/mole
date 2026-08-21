#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/IndexesFeature.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/automation/Scheduler.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSummary.h"

#include <QFile>
#include <QGuiApplication>
#include <QTest>

#include <atomic>

using namespace mole;
using namespace mole::test;

/// Nothing the interface does may read the index on the thread that draws it.
///
/// The index is a database on a disk. ADR-0065 stopped a read of it queueing
/// behind a scan's writes, and left the other half: the read still happened on
/// the drawing thread, where its duration is decided by whatever else is
/// touching the database. MOLE-264 named seven call sites, five of them property
/// getters that QML evaluates whenever anything they depend on changes -- and an
/// eighth arrived during the session that fixed the other seven, which is the
/// argument for a rule the code enforces rather than a list somebody maintains.
///
/// So `IndexDatabase::doNotReadFrom()` names the drawing thread and warns when a
/// read comes from it, and these tests walk each route the interface has and
/// assert it stays quiet. The routes are named one per data row, so a ninth is a
/// line somebody adds on purpose -- and `theGuardItselfNoticesADirectRead` is
/// here because a guard that has stopped working would let every one of them
/// pass without a word. See ADR-0066.
class TestIndexOffTheDrawingThread : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theGuardItselfNoticesADirectRead();
    void nothingTheInterfaceDoesReadsTheIndexOnTheDrawingThread_data();
    void nothingTheInterfaceDoesReadsTheIndexOnTheDrawingThread();
    void theSessionLogStillNamesTheIndexes();
    void aFolderChangeWhileAScanRunsStillAnswers();
    void forgettingAnIndexDoesNotWaitForAScanToStopWriting();
    void aRemovalThatFailsLeavesTheRowAndSaysSo();
    void anAnswerThatHasNotArrivedDoesNotSayTheFolderIsUnindexed();

private:
    /// Writes a finished scan of `label` straight into the index. Does **not**
    /// tell the snapshot, so a test can choose whether the interface knows yet.
    bool seed(const QString& label, int files = 3);
    QString fixtureUri() const;
    BrowserController* openBrowser();
    LiveSearchController* openSearch();
    IndexesController* openIndexes();

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

/// What the guard says. Matching on this rather than on a whole line, because
/// what matters is that a read happened here at all.
static const QString kComplaint = QStringLiteral("Index query on the thread that draws the window");

void TestIndexOffTheDrawingThread::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestIndexOffTheDrawingThread::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_INDEX_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("photos/a.jpg")));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/a.txt")));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
    // A scheduled run firing mid-test would make the timing its own variable.
    m_app->scheduler()->stop();
}

void TestIndexOffTheDrawingThread::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

QString TestIndexOffTheDrawingThread::fixtureUri() const
{
    return m_tree->rootUri().toString();
}

bool TestIndexOffTheDrawingThread::seed(const QString& label, int files)
{
    IndexDatabase* index = m_app->services().index;
    if (!index)
        return false;
    // Writing from this thread is what the guard now complains about, and doing it
    // here is deliberate: a test that states exactly what is in the index writes it
    // rather than scanning for it. See MOLE-274.
    UsingTheIndexOnPurpose direct(index);

    const QString uri = fixtureUri() + QLatin1Char('/') + label;
    const Result<qint64> volume = index->upsertVolume(VfsUri::fromString(uri), label);
    if (!volume.ok())
        return false;
    const Result<qint64> scan = index->beginScan(volume.value());
    if (!scan.ok())
        return false;

    QList<IndexedFile> rows;
    for (int i = 0; i < files; ++i) {
        IndexedFile row;
        row.name = QStringLiteral("file-%1.txt").arg(i);
        row.parentPath = QStringLiteral("/%1").arg(label);
        row.path = row.parentPath + QLatin1Char('/') + row.name;
        row.extension = QStringLiteral("txt");
        row.facts = { SearchFact { QStringLiteral("test.invented"), QStringLiteral("something"), 0, false } };
        rows.append(row);
    }
    if (!index->insertBatch(volume.value(), scan.value(), rows).ok())
        return false;
    return index
        ->commitScan(
            volume.value(), scan.value(), QDateTime::currentDateTime().addSecs(-86400), ScanOptions {})
        .ok();
}

BrowserController* TestIndexOffTheDrawingThread::openBrowser()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.browser"));
    return row < 0 ? nullptr : qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
}

LiveSearchController* TestIndexOffTheDrawingThread::openSearch()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    return row < 0 ? nullptr : qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
}

IndexesController* TestIndexOffTheDrawingThread::openIndexes()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.indexes"));
    return row < 0 ? nullptr : qobject_cast<IndexesController*>(m_app->tabs()->controllerAt(row));
}

void TestIndexOffTheDrawingThread::theGuardItselfNoticesADirectRead()
{
    // First, because everything below is an assertion that the guard said
    // nothing -- and a guard that has stopped working says nothing about
    // everything. This is the one test here that would still fail if
    // doNotReadFrom() were quietly removed.
    QVERIFY(seed(QStringLiteral("photos")));

    // One capture at a time: CapturedWarnings owns the message handler, so two
    // of them alive together would leave the wrong one installed.
    {
        CapturedWarnings warnings;
        const Result<QList<IndexVolume>> read = m_app->services().index->volumes();
        QVERIFY(read.ok());
        QVERIFY2(warnings.contains(kComplaint),
            qPrintable(QStringLiteral("the guard did not notice a read from this thread: %1")
                           .arg(warnings.joined())));
        QVERIFY2(warnings.contains(QStringLiteral("volumes()")), qPrintable(warnings.joined()));
    }

    // And it stands down where a test says it is asking about storage on
    // purpose, or every suite that inspects the index would be shouted at.
    {
        CapturedWarnings quiet;
        {
            UsingTheIndexOnPurpose direct(m_app->services().index);
            QVERIFY(m_app->services().index->volumes().ok());
        }
        QVERIFY2(!quiet.contains(kComplaint), qPrintable(quiet.joined()));
    }

    // Standing down is scoped: the guard is back afterwards.
    {
        CapturedWarnings again;
        QVERIFY(m_app->services().index->volumes().ok());
        QVERIFY2(again.contains(kComplaint), qPrintable(again.joined()));
    }
}

void TestIndexOffTheDrawingThread::nothingTheInterfaceDoesReadsTheIndexOnTheDrawingThread_data()
{
    // One row per call site MOLE-264 named. The name is the route a user takes,
    // and the comment is the code that used to ask the database.
    QTest::addColumn<QString>("route");
    QTest::newRow("the Indexes tab lists what is indexed") << QStringLiteral("indexes tab");
    QTest::newRow("a folder change asks what covers the folder") << QStringLiteral("folder change");
    QTest::newRow("the search form lists the volumes to pick from") << QStringLiteral("search volumes");
    QTest::newRow("the search form says whether the index covers this") << QStringLiteral("search coverage");
    QTest::newRow("the search form offers the fields the index can answer")
        << QStringLiteral("search fields");
    // A write, and the last one the interface still made here. It belongs in this
    // list because the guard covers writers too since MOLE-274.
    QTest::newRow("forgetting an index") << QStringLiteral("forget");
}

void TestIndexOffTheDrawingThread::nothingTheInterfaceDoesReadsTheIndexOnTheDrawingThread()
{
    QFETCH(QString, route);

    // Something in the index, so every route below has an answer to find. A route
    // that stays quiet because there was nothing to look up would assert nothing.
    QVERIFY(seed(QStringLiteral("photos")));
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));
    QVERIFY2(m_app->services().indexSummary->isKnown(), "the snapshot has to have an answer");
    QVERIFY(!m_app->services().indexSummary->volumes().isEmpty());

    const QString photos = fixtureUri() + QStringLiteral("/photos");

    CapturedWarnings warnings;

    if (route == QStringLiteral("indexes tab")) {
        IndexesController* tab = openIndexes();
        QVERIFY(tab);
        QCOMPARE(tab->volumeCount(), 1);
        QVERIFY(!tab->volumes().isEmpty());
    } else if (route == QStringLiteral("folder change")) {
        BrowserController* browser = openBrowser();
        QVERIFY(browser);
        // The hot one: connected to locationChanged, so it runs on every folder
        // change rather than once at startup.
        browser->navigateActive(photos);
        QVERIFY(waitFor([browser] { return browser->isIndexed(); }));
        QVERIFY(browser->indexedText().contains(QStringLiteral("indexed")));
    } else if (route == QStringLiteral("search volumes")) {
        LiveSearchController* search = openSearch();
        QVERIFY(search);
        QVERIFY2(search->volumeLabels().size() > 1, "the seeded volume is offered, beside All volumes");
    } else if (route == QStringLiteral("search coverage")) {
        LiveSearchController* search = openSearch();
        QVERIFY(search);
        search->setRootUri(photos);
        QVERIFY(waitFor([search] { return search->indexCoversRoot(); }));
        QVERIFY(search->indexNote().contains(QStringLiteral("indexed")));
    } else if (route == QStringLiteral("search fields")) {
        LiveSearchController* search = openSearch();
        QVERIFY(search);
        search->setRootUri(photos);
        QVERIFY(waitFor([search] { return !search->factKeys().isEmpty(); }));
        QCOMPARE(search->factKeys(), QStringList { QStringLiteral("test.invented") });
        // coverageNote() calls factKeys() again, which is what made a per-call
        // task impossible and a snapshot the only answer.
        QVERIFY(!search->coverageNote().isEmpty());
    } else if (route == QStringLiteral("forget")) {
        IndexesController* tab = openIndexes();
        QVERIFY(tab);
        QCOMPARE(tab->volumeCount(), 1);
        const qint64 id = tab->volumes().first().toMap().value(QStringLiteral("id")).toLongLong();
        QVERIFY(tab->forget(id));
        // The row goes when the removal has happened, not when it was asked for.
        QVERIFY(
            waitFor([this, tab] { return m_app->tasks()->activeCount() == 0 && tab->volumeCount() == 0; }));
    } else {
        QFAIL(qPrintable(QStringLiteral("unknown route %1").arg(route)));
    }

    QVERIFY2(!warnings.contains(kComplaint),
        qPrintable(
            QStringLiteral("%1 read the index on the drawing thread: %2").arg(route, warnings.joined())));
}

void TestIndexOffTheDrawingThread::theSessionLogStillNamesTheIndexes()
{
    // The eighth site, and the one that proves a written-down list of them goes
    // out of date: this line was added during the session that fixed the other
    // seven, six lines below a comment naming this very fault. It has to keep
    // saying what is in the index -- from the snapshot, and so a moment later.
    QVERIFY(seed(QStringLiteral("photos")));

    m_app.reset();
    CapturedWarnings log(QtInfoMsg);
    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
    m_app->scheduler()->stop();

    QVERIFY2(waitFor([&log] { return log.contains(QStringLiteral("Indexes:")); }),
        qPrintable(QStringLiteral("no index line arrived: %1").arg(log.joined())));
    QVERIFY2(log.contains(QStringLiteral("photos")), qPrintable(log.joined()));
    QVERIFY2(log.contains(QStringLiteral("files")), qPrintable(log.joined()));
    QVERIFY2(!log.contains(kComplaint),
        qPrintable(QStringLiteral("the log read the index on the drawing thread: %1").arg(log.joined())));
}

void TestIndexOffTheDrawingThread::aFolderChangeWhileAScanRunsStillAnswers()
{
    // A tree big enough that the scan outlasts what is done to the interface
    // while it runs. Not a clock: the assertion at the end is that the scan was
    // still going, so a machine fast enough to finish it early fails loudly
    // rather than passing for the wrong reason.
    // Sized for margin rather than for the minimum that works: the assertion at
    // the end is that the scan had not finished, so a tree the machine can walk
    // faster than the interface can be driven turns this into a flaky test
    // rather than a wrong one. Three thousand files is a few hundred
    // milliseconds of walking against a couple of navigations.
    for (int i = 0; i < 3000; ++i)
        QVERIFY(m_tree->writeFile(QStringLiteral("big/f%1.txt").arg(i)));

    QVERIFY(seed(QStringLiteral("photos")));
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));

    BrowserController* browser = openBrowser();
    QVERIFY(browser);

    LiveSearchController* scanner = openSearch();
    QVERIFY(scanner);
    scanner->scanDirectory(fixtureUri() + QStringLiteral("/big"), QStringLiteral("big"));
    QVERIFY2(waitFor([this] { return m_app->tasks()->activeCount() > 0; }), "the scan has to be running");

    CapturedWarnings warnings;

    // Everything the interface would do while somebody works through a scan.
    const QString photos = fixtureUri() + QStringLiteral("/photos");
    browser->navigateActive(photos);
    QVERIFY2(waitFor([browser] { return browser->isIndexed(); }),
        "the folder facts still arrive while a scan is writing");
    QVERIFY(browser->indexedText().contains(QStringLiteral("indexed")));

    browser->navigateActive(fixtureUri() + QStringLiteral("/docs"));
    QVERIFY(waitFor([browser] { return !browser->isIndexed(); }));

    QVERIFY2(m_app->tasks()->activeCount() > 0,
        "the scan finished before the interface was asked anything, so this proved nothing");
    QVERIFY2(!warnings.contains(kComplaint), qPrintable(warnings.joined()));
}

void TestIndexOffTheDrawingThread::forgettingAnIndexDoesNotWaitForAScanToStopWriting()
{
    // The claim, and it is a condition rather than a clock: **forget() returns
    // while a writer is still working.** Writers are serialised on purpose
    // (ADR-0065), so a removeVolume() called on this thread waits for whatever
    // transaction a scan has open -- and a DELETE over a large volume is not quick
    // either. It was the last index write the interface still made here.
    //
    // Under the old code this blocks until the batch below is committed, by which
    // time writeDone is set, so the assertion fails with a thousandfold margin
    // rather than by a hair. See MOLE-274.
    QVERIFY(seed(QStringLiteral("photos")));
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->volumeCount(), 1);
    const qint64 id = tab->volumes().first().toMap().value(QStringLiteral("id")).toLongLong();

    // A second volume with a long write in flight, so the writer lock is held by
    // something that is not the volume being forgotten.
    constexpr int kRows = 50000;
    qint64 other = -1;
    qint64 generation = -1;
    {
        UsingTheIndexOnPurpose direct(m_app->services().index);
        const Result<qint64> made = m_app->services().index->upsertVolume(
            VfsUri::fromString(fixtureUri() + QStringLiteral("/docs")), QStringLiteral("docs"));
        QVERIFY(made.ok());
        other = made.value();
        const Result<qint64> scan = m_app->services().index->beginScan(other);
        QVERIFY(scan.ok());
        generation = scan.value();
    }

    QList<IndexedFile> rows;
    rows.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        IndexedFile row;
        row.name = QStringLiteral("f%1.txt").arg(i);
        row.parentPath = QStringLiteral("/docs");
        row.path = row.parentPath + QLatin1Char('/') + row.name;
        row.extension = QStringLiteral("txt");
        rows.append(std::move(row));
    }

    std::atomic_bool writeStarted { false };
    std::atomic_bool writeDone { false };
    auto* writer = new ScriptedTask(QStringLiteral("a long write"), [&](ScriptedTask&) {
        writeStarted = true;
        m_app->services().index->insertBatch(other, generation, rows);
        writeDone = true;
    });
    m_app->services().tasks->submit(writer);
    QVERIFY(waitFor([&writeStarted] { return writeStarted.load(); }));

    CapturedWarnings warnings;
    QVERIFY(tab->forget(id));
    const bool writerStillWorking = !writeDone.load();

    QVERIFY(waitFor([&writeDone] { return writeDone.load(); }, 30000));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));

    QVERIFY2(writerStillWorking,
        "forget() did not return until the write had finished -- it is waiting for the writer lock on "
        "the thread that draws the window");
    QVERIFY2(!warnings.contains(kComplaint), qPrintable(warnings.joined()));

    // And it did happen: the row goes once the removal has landed, and the one
    // that goes is the one that was asked for.
    const auto labelsNow = [tab] {
        QStringList out;
        for (const QVariant& row : tab->volumes())
            out.append(row.toMap().value(QStringLiteral("label")).toString());
        out.sort();
        return out;
    };
    QVERIFY2(waitFor([&labelsNow] { return labelsNow() == QStringList { QStringLiteral("docs") }; }),
        qPrintable(QStringLiteral("the list settled on [%1], not on [docs]")
                       .arg(labelsNow().join(QStringLiteral(", ")))));
}

void TestIndexOffTheDrawingThread::aRemovalThatFailsLeavesTheRowAndSaysSo()
{
    // A removal that cannot happen must not look like one that did. Closing the
    // index is the cheapest way to make a write fail for certain -- open() and
    // close() are outside the guard by construction, because they run on this
    // thread when the application starts and stops.
    QVERIFY(seed(QStringLiteral("photos")));
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->volumeCount(), 1);
    const qint64 id = tab->volumes().first().toMap().value(QStringLiteral("id")).toLongLong();

    QString said;
    connect(m_app->services().events, &EventBus::notificationPosted, this,
        [&said](EventBus::Severity, const QString& title, const QString& detail) {
            said = title + QLatin1Char(' ') + detail;
        });

    m_app->services().index->close();
    QVERIFY(tab->forget(id));
    QVERIFY2(waitFor([&said] { return !said.isEmpty(); }),
        "a removal that failed has to say so rather than fail silently");
    QVERIFY2(said.contains(QStringLiteral("photos")), qPrintable(said));

    // And the row is still there, because the index it names still is.
    drainEvents();
    QCOMPARE(tab->volumeCount(), 1);
}

void TestIndexOffTheDrawingThread::anAnswerThatHasNotArrivedDoesNotSayTheFolderIsUnindexed()
{
    // The failure this design is most exposed to. A snapshot that has not read
    // yet knows nothing, and if that were rendered as "nothing is indexed" the
    // interface would make a confident false statement about a folder that *is*
    // -- worse than the stall it replaced, because a stall is visibly the
    // application's fault and this would send somebody to re-scan a tree that is
    // already there. So there are three states and not two. See ADR-0066.
    QVERIFY(seed(QStringLiteral("photos")));
    // Deliberately not refreshed: the rows are in the index and the interface
    // has not been told.
    QVERIFY2(
        !m_app->services().indexSummary->volumes().isEmpty() || !m_app->services().indexSummary->isKnown(),
        "this test needs a snapshot that has not seen the seed");

    BrowserController* browser = openBrowser();
    QVERIFY(browser);
    const QString photos = fixtureUri() + QStringLiteral("/photos");
    browser->navigateActive(photos);
    drainEvents();

    // It may already know -- the snapshot reads once at startup and the seed
    // races it. What must never happen is a claim either way.
    if (!browser->isIndexed()) {
        QVERIFY2(browser->indexedText().isEmpty(),
            qPrintable(QStringLiteral("an unanswered index made a claim: %1").arg(browser->indexedText())));
    }

    // And once the answer lands, the tag appears without the user doing anything.
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));
    QVERIFY(waitFor([browser] { return browser->isIndexed(); }));
    QVERIFY(browser->indexedText().contains(QStringLiteral("indexed")));

    // The search form is the same: it does not claim coverage it has not been
    // told about, and it does not deny it either -- indexNote() is empty until
    // there is something true to say.
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    search->setRootUri(photos);
    QVERIFY(waitFor([search] { return search->indexCoversRoot(); }));
    QVERIFY(search->indexNote().contains(QStringLiteral("indexed")));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestIndexOffTheDrawingThread testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_IndexOffTheDrawingThread.moc"
