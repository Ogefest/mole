#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/IndexScanJob.h"
#include "plugins/builtin/IndexesFeature.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The list of indexes, which did not exist: an index is a claim about a tree
/// that goes quietly out of date, and the only place one used to appear was a
/// dropdown inside the search form, as a label and a count. See MOLE-230.
class TestIndexesTab : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void everyIndexIsListedWithWhatIsInItAndHowOldItIs();
    void anIndexOnAClockSaysSoAndTheOthersSayTheyAreNot();
    void aVolumeIndexedBeforeTheOptionsWereRecordedSaysItCannotTell();
    void nothingIndexedReadsAsNothingRatherThanAsAnEmptyList();
    void theListFollowsAScanThatFinishesElsewhere();
    void rescanningARowRepeatsTheKindOfScanTheVolumeRecords();
    void aFullRescanWalksEverything();
    void removingARowsScheduleLeavesTheVolumeAndTakesTheRule();
    void forgettingARowTakesTheIndexAndItsSchedule();
    void aRunningScanShowsOnItsRowAndStoppingItChangesNothing();

private:
    IndexesController* openIndexes();
    /// Writes a volume straight into the index, so a test can state exactly what
    /// kind of scan built it without walking anything.
    bool seed(const QString& uri, const QString& label, const ScanOptions& options, int files = 2);
    /// The row for `label`, or an empty map when the list has no such row.
    QVariantMap rowFor(IndexesController* tab, const QString& label) const;

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestIndexesTab::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestIndexesTab::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SCHEDULE_PATH")));
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
    // The real poll would fire during the test and make the timing its own
    // variable.
    m_app->scheduler()->stop();
}

void TestIndexesTab::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

IndexesController* TestIndexesTab::openIndexes()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.indexes"));
    return row < 0 ? nullptr : qobject_cast<IndexesController*>(m_app->tabs()->controllerAt(row));
}

bool TestIndexesTab::seed(const QString& uri, const QString& label, const ScanOptions& options, int files)
{
    IndexDatabase* index = m_app->services().index;
    if (!index)
        return false;
    // Seeding is a write from the guarded thread, on purpose. See MOLE-274.
    UsingTheIndexOnPurpose direct(index);

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
        row.path = QStringLiteral("/%1/file-%2.txt").arg(label).arg(i);
        row.parentPath = QStringLiteral("/%1").arg(label);
        row.extension = QStringLiteral("txt");
        rows.append(row);
    }
    if (!index->insertBatch(volume.value(), scan.value(), rows).ok())
        return false;
    // Dated a little back, so "scanned just now" is not what every row says.
    if (!index
             ->commitScan(
                 volume.value(), scan.value(), QDateTime::currentDateTime().addSecs(-3 * 86400), options)
             .ok())
        return false;

    // Rows written by hand are written somewhere nothing is looking: the tab
    // reads the interface's snapshot, and in the application a finished scan is
    // what tells it to read again. See ADR-0066.
    return refreshIndexSummary(m_app->services().indexSummary);
}

QVariantMap TestIndexesTab::rowFor(IndexesController* tab, const QString& label) const
{
    const QVariantList rows = tab->volumes();
    for (const QVariant& entry : rows) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("label")).toString() == label)
            return row;
    }
    return {};
}

/// The five facts somebody opening this list is asking for: what it covers, how
/// old it is, how big it is, what kind of scan built it, and whether anything is
/// keeping it fresh.
void TestIndexesTab::everyIndexIsListedWithWhatIsInItAndHowOldItIs()
{
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();

    ScanOptions everything;
    everything.incremental = true;
    everything.metadata = true;
    everything.archives = true;
    QVERIFY(seed(photos, QStringLiteral("photos"), everything, 4));
    QVERIFY(seed(docs, QStringLiteral("docs"), ScanOptions {}, 2));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->volumeCount(), 2);

    const QVariantMap rich = rowFor(tab, QStringLiteral("photos"));
    QVERIFY2(!rich.isEmpty(), "an indexed tree has to appear in the list of indexes");
    QCOMPARE(rich.value(QStringLiteral("rootUri")).toString(), photos);
    QCOMPARE(rich.value(QStringLiteral("entryCount")).toLongLong(), 4);
    // The age, in the same words the search form uses for it.
    QVERIFY2(rich.value(QStringLiteral("scannedText")).toString().contains(QStringLiteral("3 days ago")),
        qPrintable(rich.value(QStringLiteral("scannedText")).toString()));
    // And what kind of scan built it, which no proxy could answer.
    QVERIFY(rich.value(QStringLiteral("kindKnown")).toBool());
    QVERIFY(rich.value(QStringLiteral("hasMetadata")).toBool());
    QVERIFY(rich.value(QStringLiteral("hasArchives")).toBool());
    QVERIFY2(rich.value(QStringLiteral("kindText")).toString().contains(QStringLiteral("say about")),
        qPrintable(rich.value(QStringLiteral("kindText")).toString()));

    const QVariantMap bare = rowFor(tab, QStringLiteral("docs"));
    QVERIFY(!bare.isEmpty());
    QCOMPARE(bare.value(QStringLiteral("entryCount")).toLongLong(), 2);
    QVERIFY(bare.value(QStringLiteral("kindKnown")).toBool());
    QVERIFY(!bare.value(QStringLiteral("hasMetadata")).toBool());
    QCOMPARE(bare.value(QStringLiteral("kindText")).toString(), QStringLiteral("names only"));

    // Six entries across two indexes, which is what an index costs in one number.
    QCOMPARE(tab->totalEntriesText(), QLocale().toString(6));
}

/// A row that is on a clock says so here rather than making somebody cross-check
/// Automation, where an index with no rule does not appear at all.
void TestIndexesTab::anIndexOnAClockSaysSoAndTheOthersSayTheyAreNot()
{
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();
    QVERIFY(seed(photos, QStringLiteral("photos"), ScanOptions {}));
    QVERIFY(seed(docs, QStringLiteral("docs"), ScanOptions {}));

    // Through the form that makes them, rather than a rule written by hand.
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* form = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(form);
    QVERIFY(!form->scheduleScan(photos, 24 * 3600).isEmpty());

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->scheduledCount(), 1);

    const QVariantMap kept = rowFor(tab, QStringLiteral("photos"));
    QVERIFY(kept.value(QStringLiteral("scheduled")).toBool());
    QCOMPARE(kept.value(QStringLiteral("scheduleText")).toString(), QStringLiteral("Every day"));
    QVERIFY2(!kept.value(QStringLiteral("nextDueText")).toString().isEmpty(),
        "a row on a clock has to say when it next runs");

    const QVariantMap loose = rowFor(tab, QStringLiteral("docs"));
    QVERIFY(!loose.value(QStringLiteral("scheduled")).toBool());
    QCOMPARE(loose.value(QStringLiteral("scheduleText")).toString(), QStringLiteral("not on a clock"));
    QVERIFY(loose.value(QStringLiteral("nextDueText")).toString().isEmpty());
}

/// Not known is a third answer and not a polite no. A list that told somebody the
/// index they built with metadata last week has none would be worse than the
/// dropdown it replaces.
void TestIndexesTab::aVolumeIndexedBeforeTheOptionsWereRecordedSaysItCannotTell()
{
    IndexDatabase* index = m_app->services().index;
    QVERIFY(index);
    const QString uri = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    // A volume with no finished scan behind it is what a row written by an older
    // version looks like from here: no recorded options at all.
    QVERIFY(index->upsertVolume(VfsUri::fromString(uri), QStringLiteral("older")).ok());
    // Written by hand, so nothing has told the interface's snapshot to look.
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const QVariantMap row = rowFor(tab, QStringLiteral("older"));
    QVERIFY(!row.isEmpty());
    QVERIFY2(!row.value(QStringLiteral("kindKnown")).toBool(),
        "a volume with no recorded options cannot claim to have none");
    QVERIFY2(row.value(QStringLiteral("kindText")).toString().contains(QStringLiteral("not known")),
        qPrintable(row.value(QStringLiteral("kindText")).toString()));
    QVERIFY2(row.value(QStringLiteral("scannedText")).toString().contains(QStringLiteral("never")),
        qPrintable(row.value(QStringLiteral("scannedText")).toString()));
}

void TestIndexesTab::nothingIndexedReadsAsNothingRatherThanAsAnEmptyList()
{
    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->volumeCount(), 0);
    QVERIFY(tab->volumes().isEmpty());
    QVERIFY2(tab->subtitle().contains(QStringLiteral("nothing indexed")), qPrintable(tab->subtitle()));
}

/// A scan that finishes anywhere -- a search tab, the folder menu, the nightly
/// rule -- shows up here without the tab being reopened.
void TestIndexesTab::theListFollowsAScanThatFinishesElsewhere()
{
    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    QCOMPARE(tab->volumeCount(), 0);

    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    QVERIFY(seed(photos, QStringLiteral("photos"), ScanOptions {}));
    m_app->services().events->postIndexUpdated(-1, 2);

    QVERIFY(waitFor([tab] { return tab->volumeCount() == 1; }));
}

/// The options a volume records are what a rescan of it asks for. Anything else
/// makes a tree indexed one way on Monday indexed another way on Tuesday, which
/// is the fault the whole epic is about.
void TestIndexesTab::rescanningARowRepeatsTheKindOfScanTheVolumeRecords()
{
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    ScanOptions everything;
    everything.incremental = true;
    everything.metadata = true;
    everything.archives = true;
    QVERIFY(seed(photos, QStringLiteral("photos"), everything));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const qint64 id = rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("id")).toLongLong();
    QVERIFY(id >= 0);

    QVERIFY(tab->rescan(id, false));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));

    // Read back off the volume, which is where a finished scan records what it
    // was asked for.
    const QVariantMap after = rowFor(tab, QStringLiteral("photos"));
    QVERIFY2(after.value(QStringLiteral("hasMetadata")).toBool(),
        "a rescan must not quietly drop what the volume was indexed with");
    QVERIFY(after.value(QStringLiteral("hasArchives")).toBool());
    // And it really re-walked the tree rather than only touching the row.
    QVERIFY(after.value(QStringLiteral("entryCount")).toLongLong() > 0);
}

/// The distinction the scan dialog makes, on a row: full is what somebody reaches
/// for when they suspect the index, and it has to keep nothing.
void TestIndexesTab::aFullRescanWalksEverything()
{
    // A subfolder, because carrying forward is what happens to a *subtree* whose
    // date has not moved -- a flat folder has nothing to carry.
    QVERIFY(m_tree->writeFile(QStringLiteral("photos/2025/leaf.jpg")));
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    QVERIFY(seed(photos, QStringLiteral("photos"), ScanOptions {}));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const qint64 id = rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("id")).toLongLong();

    const auto lastScanKeptSomething = [this] {
        // The scan's own line is where "unchanged and kept" is said.
        TaskListModel* tasks = m_app->tasks();
        for (int row = tasks->rowCount() - 1; row >= 0; --row) {
            const QModelIndex at = tasks->index(row, 0);
            if (tasks->data(at, TaskListModel::TitleRole).toString().startsWith(QStringLiteral("Scan "))) {
                return tasks->data(at, TaskListModel::StatusTextRole)
                    .toString()
                    .contains(QStringLiteral("unchanged and kept"));
            }
        }
        return false;
    };

    // Dated back, because the index only trusts a folder that was already settled
    // when the last scan ran. Then two incremental scans: the first records the
    // dates and the second is the one with something to carry.
    const QDateTime settled = QDateTime::currentDateTime().addSecs(-3600);
    for (const QString& folder : { QStringLiteral("photos"), QStringLiteral("photos/2025") })
        QVERIFY2(m_tree->setModified(folder, settled), qPrintable(folder));

    QVERIFY(tab->rescan(id, false));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));
    QVERIFY(tab->rescan(id, false));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));
    QVERIFY2(lastScanKeptSomething(), "an incremental rescan of an unchanged tree has to keep something");

    // And the full one keeps nothing, which is the whole difference.
    QVERIFY(tab->rescan(id, true));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));
    QVERIFY2(!lastScanKeptSomething(), "a full rescan walks everything and keeps nothing");
}

void TestIndexesTab::removingARowsScheduleLeavesTheVolumeAndTakesTheRule()
{
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    QVERIFY(seed(photos, QStringLiteral("photos"), ScanOptions {}));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const qint64 id = rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("id")).toLongLong();

    QVERIFY(tab->setSchedule(id, 7 * 24 * 3600));
    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("scheduleText")).toString(),
        QStringLiteral("Every week"));
    // Incremental, whatever this volume's own last scan was: a nightly full walk
    // is hours a night for nothing.
    QVERIFY(IndexScanJob::optionsFor(m_app->schedules()->rules().first()).incremental);

    // Changed rather than ignored, the way the index dialog's picker behaves.
    QVERIFY(tab->setSchedule(id, 24 * 3600));
    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(m_app->schedules()->rules().first().intervalSeconds, 24 * 3600);

    QVERIFY(tab->setSchedule(id, 0));
    QVERIFY(m_app->schedules()->rules().isEmpty());
    // The index itself is untouched: taking it off a clock is not forgetting it.
    QCOMPARE(tab->volumeCount(), 1);
    QCOMPARE(rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("scheduleText")).toString(),
        QStringLiteral("not on a clock"));
}

/// The guide already promised the index can be deleted without losing anything
/// but time, and no interface could do it.
void TestIndexesTab::forgettingARowTakesTheIndexAndItsSchedule()
{
    const QString photos = m_tree->rootUri().child(QStringLiteral("photos")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();
    QVERIFY(seed(photos, QStringLiteral("photos"), ScanOptions {}));
    QVERIFY(seed(docs, QStringLiteral("docs"), ScanOptions {}));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const qint64 id = rowFor(tab, QStringLiteral("photos")).value(QStringLiteral("id")).toLongLong();
    QVERIFY(tab->setSchedule(id, 24 * 3600));

    // The rows are in the index before it goes, so their absence afterwards means
    // something.
    SearchQuery byName;
    byName.add(SearchPredicate::name(QStringLiteral("file-0.txt")));
    // Storage, not the interface -- so the guard is stood down for the read.
    UsingTheIndexOnPurpose direct(m_app->services().index);
    QCOMPARE(m_app->services().index->search(byName).value().size(), 2);

    QVERIFY(tab->forget(id));
    // Forgetting runs on a task, so the index has to be *seen* to have lost the
    // volume before the snapshot is asked to read again. Refreshing straight
    // after the call raced the delete: the read landed first, the row was still
    // there, and the test failed once in a parallel run and never again. On the
    // condition, never on a clock.
    QVERIFY(waitFor([&] { return m_app->services().index->volumes().value().size() == 1; }, 5000));
    // The row then goes when the snapshot has read again, which is a task round
    // trip rather than the same stack frame.
    QVERIFY(refreshIndexSummary(m_app->services().indexSummary));

    QCOMPARE(tab->volumeCount(), 1);
    QVERIFY(rowFor(tab, QStringLiteral("photos")).isEmpty());
    // Its rows are gone from a search, and the other volume's are not.
    QCOMPARE(m_app->services().index->search(byName).value().size(), 1);
    // And the rule that would have rebuilt it tonight went with it.
    QVERIFY2(m_app->schedules()->rules().isEmpty(),
        "a rule outliving the index it refreshes would rebuild what was just forgotten");
}

/// A scheduled scan starting while somebody is working is otherwise a mystery
/// slowdown: it is visible and stoppable where the indexes are listed. Stopping
/// leaves the volume as it was, which the generation swap already guarantees --
/// what is new is that it is reported.
void TestIndexesTab::aRunningScanShowsOnItsRowAndStoppingItChangesNothing()
{
    // A tree big enough that a scan of it is still going when it is looked at.
    for (int i = 0; i < 400; ++i)
        QVERIFY(m_tree->writeFile(QStringLiteral("big/branch%1/leaf.txt").arg(i)));
    const QString big = m_tree->rootUri().child(QStringLiteral("big")).toString();
    QVERIFY(seed(big, QStringLiteral("big"), ScanOptions {}));

    IndexesController* tab = openIndexes();
    QVERIFY(tab);
    const QVariantMap before = rowFor(tab, QStringLiteral("big"));
    const qint64 id = before.value(QStringLiteral("id")).toLongLong();
    const QString scannedBefore = before.value(QStringLiteral("scannedAt")).toString();
    const qint64 entriesBefore = before.value(QStringLiteral("entryCount")).toLongLong();
    QVERIFY(entriesBefore > 0);

    QVERIFY(tab->rescan(id, true));

    // On the row, waited for on the condition rather than on a clock.
    QVERIFY(waitFor(
        [&] { return rowFor(tab, QStringLiteral("big")).value(QStringLiteral("running")).toBool(); }));
    QVERIFY(tab->stopScan(id));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 30000));

    const QVariantMap after = rowFor(tab, QStringLiteral("big"));
    QVERIFY2(!after.value(QStringLiteral("running")).toBool(), "a stopped scan is not still running");
    // The previous contents, unchanged: same count, same date, still searchable.
    QCOMPARE(after.value(QStringLiteral("entryCount")).toLongLong(), entriesBefore);
    QCOMPARE(after.value(QStringLiteral("scannedAt")).toString(), scannedBefore);
    SearchQuery byName;
    byName.add(SearchPredicate::name(QStringLiteral("file-0.txt")));
    UsingTheIndexOnPurpose direct(m_app->services().index);
    QCOMPARE(m_app->services().index->search(byName).value().size(), 1);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestIndexesTab testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_IndexesTab.moc"
