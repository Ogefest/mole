#include "plugins/builtin/DuplicatesFeature.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/DuplicateGroupModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Choosing what to keep, which is the half of a duplicate scan that can lose a
/// file.
///
/// Finding duplicates is arithmetic. Proposing which of them to delete is a
/// judgement, and the one thing every judgement here has to satisfy is that
/// something survives: a rule that selected every copy of a group would offer,
/// with one click, to delete the file entirely.
class TestDuplicatesTab : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void noSelectionRuleEverProposesDeletingEveryCopy_data();
    void noSelectionRuleEverProposesDeletingEveryCopy();

    void anUnscannedTabFillsItsSpaceWithAnExplanation();
    void aTabWithNoRootsSaysHowToGiveItSome();
    void everyRootBeingSearchedGetsARowOfItsOwn();
    void aScanThatMatchedNothingFillsTheSpaceAndSaysSo();
    void resultsTakeTheSpaceTheEmptyStateWasHolding();
    void aStoppedScanKeepsWhatItFoundAndSaysItWasStopped();
    void aStoppedScanThatFoundNothingDoesNotClaimNothingMatched();

    void tickedCopiesAcrossGroupsBecomeOneSet();
    void whatTheShellAimsAtIsWhatIsTicked();
    void aSetIsMadeWithoutDeletingAnything();

    void aRuleSaysWhatItDidAcrossEveryGroup();
    void aRuleStopsBeingARuleOnceTheTicksAreEdited();
    void keepingOneCopyLeavesEveryOtherGroupAlone();
    void noPerGroupOverrideEverProposesDeletingEveryCopy();
    void theChoiceHasWeightOnTheScreen();
    void everyStrategyNameFitsInThePickerThatOffersIt();

    void confirmingEachGroupInsertsOneRowAndResetsNothing();
    void twoHundredGroupsCostTwoHundredInsertionsAndNothingElse();
    void tickingOneCopyChangesOneRowAndRebuildsNothing();
    void aGroupArrivingLeavesTheScrollPositionWhereItWas();

private:
    /// Builds the whole window. Files go in *its* fixture and not in `m_tree`:
    /// the application mounts its own, and a scan pointed at a directory the
    /// window never mounted answers with nothing while looking exactly like a
    /// scan that found nothing.
    bool startWindow();
    /// Opens a duplicates tab on `roots` and hands back its controller. Nothing is
    /// scanned -- that is the state most of these are about.
    DuplicatesController* openTabOn(const QStringList& roots);
    /// A folder inside the window's fixture, as a uri.
    QString fixtureRoot(const QString& relativePath) const;
    /// The one visible item with this objectName, or null.
    QQuickItem* shown(const QString& objectName) const;
    /// How tall the tab body is, which is the measurement "no strip above a void"
    /// is made of.
    qreal bodyHeight() const;
    /// The item under `root` whose `text` is exactly `text`. For the insides of a
    /// popup, where the delegates are built by the style and carry no objectName
    /// of ours to look them up by.
    static QQuickItem* itemShowing(QQuickItem* root, const QString& text);
    /// Services enough for a controller with no window behind it. Three of these
    /// assertions are about the model rather than about the screen, and a
    /// headless controller is the cheapest place to hold them.
    PluginServices guiless() const;
    /// `count` groups of two copies each, every group at a size of its own so the
    /// last stage has `count` buckets and confirms them one at a time.
    bool writeGroups(int count);
    /// A group no scan produced. The only way to make one arrive at a chosen
    /// instant: a scan decides for itself when it has confirmed something, and
    /// what the view costs on an arrival is the claim being made.
    static DuplicateGroup madeUpGroup(const QString& name, qint64 size);

    std::unique_ptr<QmlAppHarness> m_harness;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<EventBus> m_events;
};

void TestDuplicatesTab::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    m_vfs->addMount(mount);

    m_tasks = std::make_unique<TaskManager>();
    // The controller refuses to do anything with incomplete services, and
    // rightly: a feature that half-works because a host forgot a field is worse
    // than one that says it cannot run.
    m_index = std::make_unique<IndexDatabase>(m_tree->absolute(QStringLiteral("index.db")));
    m_events = std::make_unique<EventBus>();
}

bool TestDuplicatesTab::startWindow()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    if (!m_harness->start({}, &error)) {
        qWarning("%s", qPrintable(error));
        return false;
    }
    return true;
}

QString TestDuplicatesTab::fixtureRoot(const QString& relativePath) const
{
    return m_harness->fixtureUri() + QLatin1Char('/') + relativePath;
}

DuplicatesController* TestDuplicatesTab::openTabOn(const QStringList& roots)
{
    const int row = m_harness->app()->tabs()->openTab(QStringLiteral("core.duplicates"));
    if (row < 0)
        return nullptr;
    auto* controller = qobject_cast<DuplicatesController*>(m_harness->app()->tabs()->controllerAt(row));
    if (!controller)
        return nullptr;
    controller->setTargets(roots);
    m_harness->app()->tabs()->setCurrentIndex(row);
    m_harness->settle();
    return controller;
}

QQuickItem* TestDuplicatesTab::shown(const QString& objectName) const
{
    for (QQuickItem* candidate : m_harness->items(objectName)) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

qreal TestDuplicatesTab::bodyHeight() const
{
    QQuickItem* body = shown(QStringLiteral("duplicateBody"));
    return body ? body->height() : -1;
}

QQuickItem* TestDuplicatesTab::itemShowing(QQuickItem* root, const QString& text)
{
    if (!root)
        return nullptr;
    if (root->property("text").toString() == text)
        return root;
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = itemShowing(child, text))
            return found;
    }
    return nullptr;
}

PluginServices TestDuplicatesTab::guiless() const
{
    PluginServices services;
    services.vfs = m_vfs.get();
    services.tasks = m_tasks.get();
    services.index = m_index.get();
    services.events = m_events.get();
    return services;
}

bool TestDuplicatesTab::writeGroups(int count)
{
    for (int i = 0; i < count; ++i) {
        // A size of its own, so every group is settled in a bucket of its own.
        const QByteArray body(1024 + i * 8, static_cast<char>('a' + i % 26));
        if (!m_tree->writeFile(QStringLiteral("pile/%1/one.bin").arg(i), body))
            return false;
        if (!m_tree->writeFile(QStringLiteral("pile/%1/two.bin").arg(i), body))
            return false;
    }
    return true;
}

DuplicateGroup TestDuplicatesTab::madeUpGroup(const QString& name, qint64 size)
{
    DuplicateGroup group;
    for (int i = 0; i < 2; ++i) {
        FileEntry entry;
        entry.name = name;
        entry.uri = VfsUri::fromLocalPath(QStringLiteral("/made-up/%1/%2").arg(i).arg(name));
        entry.size = size;
        entry.modified = QDateTime::fromSecsSinceEpoch(1700000000);
        group.files.append(entry);
    }
    group.reclaimable = size;
    return group;
}

void TestDuplicatesTab::cleanup()
{
    m_harness.reset();
    m_events.reset();
    m_index.reset();
    m_tasks.reset();
    m_vfs.reset();
    m_tree.reset();
}

void TestDuplicatesTab::noSelectionRuleEverProposesDeletingEveryCopy_data()
{
    QTest::addColumn<QString>("rule");
    QTest::newRow("keep the newest") << QStringLiteral("newest");
    QTest::newRow("keep the oldest") << QStringLiteral("oldest");
    QTest::newRow("keep the shortest path") << QStringLiteral("shortest");
}

void TestDuplicatesTab::noSelectionRuleEverProposesDeletingEveryCopy()
{
    QFETCH(QString, rule);

    // Three groups with awkward shapes: two copies, three copies, and a pair
    // whose timestamps and path lengths are identical -- which is where a rule
    // that picks a winner by comparison has nothing to compare and could
    // plausibly pick none.
    const QByteArray first(4096, 'a');
    const QByteArray second(4096, 'b');
    const QByteArray third(4096, 'c');
    QVERIFY(m_tree->writeFile(QStringLiteral("one.bin"), first));
    QVERIFY(m_tree->writeFile(QStringLiteral("deep/one-copy.bin"), first));
    QVERIFY(m_tree->writeFile(QStringLiteral("two.bin"), second));
    QVERIFY(m_tree->writeFile(QStringLiteral("deep/two.bin"), second));
    QVERIFY(m_tree->writeFile(QStringLiteral("deep/deeper/two.bin"), second));
    QVERIFY(m_tree->writeFile(QStringLiteral("aa/x.bin"), third));
    QVERIFY(m_tree->writeFile(QStringLiteral("bb/x.bin"), third));

    PluginServices services;
    services.vfs = m_vfs.get();
    services.tasks = m_tasks.get();
    services.index = m_index.get();
    services.events = m_events.get();

    DuplicatesController controller(services);
    controller.setStrategyId(QStringLiteral("contents"));
    controller.setMinimumSize(1);
    controller.setTargets({ m_tree->rootUri().toString() });
    QCOMPARE(controller.roots().size(), 1);
    controller.scan();
    QVERIFY(waitFor([&] { return !controller.isScanning() && controller.hasRun(); }, 30000));
    QCOMPARE(controller.groupCount(), 3);

    if (rule == QLatin1String("newest"))
        controller.keepNewest();
    else if (rule == QLatin1String("oldest"))
        controller.keepOldest();
    else
        controller.keepShortestPath();

    const QStringList chosen = controller.selectedUris();
    const QSet<QString> selected(chosen.constBegin(), chosen.constEnd());
    QVERIFY2(!selected.isEmpty(), "a rule that selects nothing is not a rule");

    // The assertion, group by group: at least one file is not on the list. Read
    // through the model's own rows, which is where the groups live since
    // MOLE-210 -- the claim is unchanged and only the reading of it moved.
    DuplicateGroupModel* model = controller.groups();
    QVERIFY(model);
    for (int row = 0; row < model->rowCount(); ++row) {
        const QVariantList files = model->data(model->index(row), DuplicateGroupModel::FilesRole).toList();
        QVERIFY(files.size() > 1);

        int kept = 0;
        for (const QVariant& file : files) {
            if (!selected.contains(file.toMap().value(QStringLiteral("uri")).toString()))
                ++kept;
        }
        QVERIFY2(kept >= 1,
            qPrintable(
                QStringLiteral("every copy of a group was selected for deletion by \"%1\"").arg(rule)));
    }
}

// ---- the tab as somebody actually meets it ------------------------------
//
// Through the real window, because every claim here is about height: an
// invisible item is dropped from a ColumnLayout rather than reserving its space,
// so a view whose only filling item was the group list collapsed upward into a
// strip of content above a void. Nothing headless can see that.

void TestDuplicatesTab::anUnscannedTabFillsItsSpaceWithAnExplanation()
{
    QVERIFY(startWindow());
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), QByteArray(4096, 'a')));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    QVERIFY2(!controller->hasRun(), "the fixture scanned, so this proves nothing");

    // The state is on screen and says which one it is, without reading the small
    // print: a heading, not a caption.
    QQuickItem* empty = shown(QStringLiteral("duplicateEmptyState"));
    QVERIFY2(empty, "an unscanned tab shows nothing at all");
    QVERIFY2(empty->height() > 0, "the empty state is there and zero pixels tall");

    // And what it costs, which was 11px grey at the bottom of a panel -- where
    // somebody about to start a scan on a NAS was least likely to read it.
    QQuickItem* cost = shown(QStringLiteral("duplicateEmptyStateCost"));
    QVERIFY(cost);
    QVERIFY(!cost->property("text").toString().isEmpty());

    // The body claims the height whether or not there is anything to put in it.
    // This is the assertion the whole issue is about.
    QQuickItem* body = shown(QStringLiteral("duplicateBody"));
    QVERIFY(body);
    QVERIFY2(body->height() > 200,
        qPrintable(QStringLiteral("the tab body is %1 pixels tall").arg(body->height())));
}

void TestDuplicatesTab::aTabWithNoRootsSaysHowToGiveItSome()
{
    QVERIFY(startWindow());
    DuplicatesController* controller = openTabOn({});
    QVERIFY(controller);
    QCOMPARE(controller->roots().size(), 0);

    // Still fills, and still says which state it is in -- a tab opened the wrong
    // way is the one most in need of a sentence.
    QVERIFY(shown(QStringLiteral("duplicateEmptyState")));
    QVERIFY(bodyHeight() > 200);
    QVERIFY(shown(QStringLiteral("duplicateNoRoots")));
    // Nothing to scan, so nothing offering to.
    QVERIFY(!shown(QStringLiteral("duplicateEmptyStateScan")));
}

void TestDuplicatesTab::everyRootBeingSearchedGetsARowOfItsOwn()
{
    QVERIFY(startWindow());
    QVERIFY(m_harness->writeFile(QStringLiteral("one/a.bin"), QByteArray(4096, 'a')));
    QVERIFY(m_harness->writeFile(QStringLiteral("two/b.bin"), QByteArray(4096, 'b')));
    QVERIFY(m_harness->writeFile(QStringLiteral("three/c.bin"), QByteArray(4096, 'c')));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("one")),
        fixtureRoot(QStringLiteral("two")), fixtureRoot(QStringLiteral("three")) });
    QVERIFY(controller);
    QCOMPARE(controller->roots().size(), 3);

    // Three rows, not one label holding three paths joined by newlines and elided
    // in the middle -- which said less than it looked like it did, and on two
    // drives hid which drive each was on.
    QList<QQuickItem*> rows;
    for (QQuickItem* row : m_harness->items(QStringLiteral("duplicateRoot"))) {
        if (row->isVisible())
            rows.append(row);
    }
    QCOMPARE(rows.size(), 3);
    for (QQuickItem* row : std::as_const(rows))
        QVERIFY2(row->height() > 0, "a root row with no height is a root nobody can read");
}

void TestDuplicatesTab::aScanThatMatchedNothingFillsTheSpaceAndSaysSo()
{
    // Three files, all different, so the scan finishes with nothing.
    QVERIFY(startWindow());
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/a.bin"), QByteArray(4096, 'a')));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/b.bin"), QByteArray(4096, 'b')));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/c.bin"), QByteArray(4096, 'c')));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("contents"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 0);

    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateNoMatchState")) != nullptr; }));
    QVERIFY(bodyHeight() > 200);
    // And the state it left behind is that one, not the one it started in.
    QVERIFY(!shown(QStringLiteral("duplicateEmptyState")));
}

void TestDuplicatesTab::resultsTakeTheSpaceTheEmptyStateWasHolding()
{
    const QByteArray same(4096, 'a');
    QVERIFY(startWindow());
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    const qreal beforeScan = bodyHeight();
    QVERIFY(beforeScan > 200);

    controller->setStrategyId(QStringLiteral("contents"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 1);

    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateGroupList")) != nullptr; }));
    QVERIFY(!shown(QStringLiteral("duplicateEmptyState")));

    // The body is still the body. It gives up exactly the height of the keep panel
    // -- which only exists once there is something to keep -- and its own spacing,
    // and nothing else. Said as arithmetic rather than as a tolerance, because a
    // tolerance is a number that has to be revisited every time a control above
    // the body changes size, and revisiting it means nobody is checking the claim
    // any more.
    QVERIFY2(bodyHeight() > 200, qPrintable(QStringLiteral("the body is %1 tall").arg(bodyHeight())));
    const qreal spacing = 10; // the body column's, from DuplicatesView.qml
    // Waited for rather than read once: the column re-lays out a frame after the
    // panel appears, so reading straight after the list became visible catches the
    // heights mid-move.
    QVERIFY2(m_harness->until([this, beforeScan, spacing] {
        QQuickItem* panel = shown(QStringLiteral("duplicateKeepPanel"));
        return panel && qAbs(bodyHeight() + panel->height() + spacing - beforeScan) <= 1;
    }),
        qPrintable(QStringLiteral("the body went from %1 to %2 while the keep panel took %3")
                       .arg(beforeScan)
                       .arg(bodyHeight())
                       .arg(shown(QStringLiteral("duplicateKeepPanel"))
                               ? shown(QStringLiteral("duplicateKeepPanel"))->height()
                               : -1)));

    // And the list fills it, rather than sitting at its natural height with a void
    // underneath -- which is the same fault one state along.
    QQuickItem* list = shown(QStringLiteral("duplicateGroupList"));
    QVERIFY(list);
    QCOMPARE(list->height(), bodyHeight());
}

void TestDuplicatesTab::aStoppedScanKeepsWhatItFoundAndSaysItWasStopped()
{
    // Ten groups, each at a size of its own, so the last stage has ten buckets and
    // stopping after the first leaves nine.
    QVERIFY(startWindow());
    for (int i = 0; i < 10; ++i) {
        const QByteArray body(1024 + i, 'x');
        QVERIFY(m_harness->writeFile(QStringLiteral("pile/%1/one.bin").arg(i), body));
        QVERIFY(m_harness->writeFile(QStringLiteral("pile/%1/two.bin").arg(i), body));
    }

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);

    // Stopped by the data, on the scan's own thread: the moment the first group is
    // confirmed, before the task has looked at its next bucket. Asking from this
    // thread instead would be asking a clock -- the window wakes on a 15 ms tick,
    // by which time a scan of twenty small files has long since finished, and the
    // test would prove nothing while looking as though it had.
    //
    // The task is caught as it is submitted, which is the only moment anything
    // outside the controller has a pointer to it.
    connect(m_harness->app()->services().tasks, &TaskManager::taskAppended, this, [](Task* task) {
        auto* scan = qobject_cast<FindDuplicatesTask*>(task);
        if (!scan)
            return;
        connect(
            scan, &FindDuplicatesTask::groupFound, scan,
            [scan](const DuplicateGroup&, int) { scan->requestCancel(); }, Qt::DirectConnection);
    });

    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));

    QVERIFY2(controller->wasCancelled(), "the scan ran to the end, so this proved nothing");
    QVERIFY2(controller->groupCount() > 0, "a stopped scan threw away what it had already confirmed");
    QVERIFY2(controller->groupCount() < 10, "the scan finished everything, so it was not stopped early");
    // And says so rather than reading as a completed scan. "no duplicates found"
    // or a bare count would both claim the tree was searched.
    QVERIFY2(controller->summary().contains(QStringLiteral("stopped")),
        qPrintable(QStringLiteral("the tab says \"%1\"").arg(controller->summary())));
    // What it did find is still on screen, in the results state.
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateGroupList")) != nullptr; }));
}

void TestDuplicatesTab::aStoppedScanThatFoundNothingDoesNotClaimNothingMatched()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/two.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);

    // Stopped as it is submitted, which is before the pool can have read a byte --
    // so there is no clock in this either.
    connect(m_harness->app()->services().tasks, &TaskManager::taskAppended, this, [](Task* task) {
        if (qobject_cast<FindDuplicatesTask*>(task))
            task->requestCancel();
    });
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QVERIFY(controller->wasCancelled());
    QCOMPARE(controller->groupCount(), 0);

    // "Nothing matched" is a claim about the tree, and only a scan that ran to the
    // end may make it. This one searched almost none of it.
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateNoMatchText")) != nullptr; }));
    const QString said = shown(QStringLiteral("duplicateNoMatchText"))->property("text").toString();
    QVERIFY2(!said.contains(QStringLiteral("Nothing matched")), qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("Stopped")), qPrintable(said));
}

// ---- the other way out --------------------------------------------------
//
// Finding duplicates is locating. What to do with what was found is a separate
// question, and until now this tab had exactly one answer to it -- the
// irreversible one.

void TestDuplicatesTab::tickedCopiesAcrossGroupsBecomeOneSet()
{
    QVERIFY(startWindow());
    const QByteArray first(4096, 'a');
    const QByteArray second(9000, 'b');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/two.bin"), second));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/two-copy.bin"), second));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 2);

    // One copy out of each group, so the set has to span both -- a set built from
    // one group would pass an assertion about "the ticked copies" while missing
    // the thing that makes this useful.
    controller->toggle(fixtureRoot(QStringLiteral("pile/deep/one-copy.bin")));
    controller->toggle(fixtureRoot(QStringLiteral("pile/deep/two-copy.bin")));
    QCOMPARE(controller->selectedCount(), 2);

    const QString id = controller->buildSetFromTicked(QStringLiteral("the pile"));
    QVERIFY2(!id.isEmpty(), "no set was made");

    const FileSet built = m_harness->app()->services().sets->set(id);
    QVERIFY(built.isValid());
    QCOMPARE(built.name, QStringLiteral("the pile"));
    QStringList members = built.uris;
    members.sort();
    QStringList expected { fixtureRoot(QStringLiteral("pile/deep/one-copy.bin")),
        fixtureRoot(QStringLiteral("pile/deep/two-copy.bin")) };
    expected.sort();
    QCOMPARE(members, expected);
}

void TestDuplicatesTab::whatTheShellAimsAtIsWhatIsTicked()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deeper/one-again.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 1);

    // Nothing ticked is nothing aimed at. A tab that answered with everything it
    // had found would turn an operation invoked by accident into one aimed at the
    // whole scan.
    QVERIFY(m_harness->app()->currentTargets().isEmpty());

    controller->toggle(fixtureRoot(QStringLiteral("pile/deep/one-copy.bin")));
    controller->toggle(fixtureRoot(QStringLiteral("pile/deeper/one-again.bin")));

    // The shell asks the current tab what it is aimed at, by name and not by type,
    // and gets the ticked copies. This is the part that makes the separation real
    // rather than cosmetic: no operation has a special case for this view, and it
    // is the same list a set is built from.
    QStringList aimed = m_harness->app()->currentTargets();
    aimed.sort();
    QStringList ticked = controller->targetUris();
    ticked.sort();
    QCOMPARE(aimed, ticked);
    QCOMPARE(aimed.size(), 2);
    QVERIFY(aimed.contains(fixtureRoot(QStringLiteral("pile/deep/one-copy.bin"))));
    QVERIFY(!aimed.contains(fixtureRoot(QStringLiteral("pile/one.bin"))));

    // And a set built from the same ticks holds exactly that.
    const QString id = controller->buildSetFromTicked({});
    QVERIFY(!id.isEmpty());
    QStringList members = m_harness->app()->services().sets->set(id).uris;
    members.sort();
    QCOMPARE(members, aimed);
}

void TestDuplicatesTab::aSetIsMadeWithoutDeletingAnything()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));

    controller->keepNewest();
    QVERIFY(controller->selectedCount() > 0);
    QVERIFY(!controller->buildSetFromTicked({}).isEmpty());
    m_harness->settle();

    // Making a set is not a way of deleting things quietly. Both copies are still
    // there, and the results are still on screen to do something else with.
    QVERIFY(QFile::exists(QDir(m_harness->fixturePath()).filePath(QStringLiteral("pile/one.bin"))));
    QVERIFY(QFile::exists(QDir(m_harness->fixturePath()).filePath(QStringLiteral("pile/deep/one-copy.bin"))));
    QCOMPARE(controller->groupCount(), 1);
}

// ---- choosing what to keep ----------------------------------------------
//
// The hard half, and the one the view used to give the least weight of anything
// on the screen: a rule was applied to every group at once and the only evidence
// it had been was a count in a corner.

void TestDuplicatesTab::aRuleSaysWhatItDidAcrossEveryGroup()
{
    QVERIFY(startWindow());
    const QByteArray first(4096, 'a');
    const QByteArray second(9000, 'b');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/two.bin"), second));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/two-copy.bin"), second));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 2);
    QCOMPARE(controller->copyCount(), 4);

    // Before any rule: nothing ticked, nothing claimed.
    QVERIFY(controller->ruleText().isEmpty());
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateRuleText")) != nullptr; }));
    QVERIFY(shown(QStringLiteral("duplicateRuleText"))
                ->property("text")
                .toString()
                .contains(QStringLiteral("Nothing is ticked")));

    controller->keepNewest();
    QCOMPARE(controller->ruleText(), QStringLiteral("Keeping the newest of each group"));
    // Two groups, one copy kept in each, so two ticked out of four -- and the
    // sentence says both numbers, because "2 ticked" alone is not something a
    // rule can be checked against.
    QCOMPARE(controller->selectedCount(), 2);
    m_harness->settle();
    const QString said = shown(QStringLiteral("duplicateRuleText"))->property("text").toString();
    QVERIFY2(said.contains(QStringLiteral("Keeping the newest")), qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("2 of 4 copies")), qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("removal")), qPrintable(said));

    // And every group shows which of its copies is being kept, so fifty of them
    // can be checked by scrolling rather than by counting ticks.
    QStringList marks;
    for (QQuickItem* mark : m_harness->items(QStringLiteral("duplicateKeepMark"))) {
        if (mark->isVisible())
            marks.append(mark->property("text").toString());
    }
    QCOMPARE(marks.count(QStringLiteral("keeping")), 2);
    QCOMPARE(marks.count(QStringLiteral("remove")), 2);
}

void TestDuplicatesTab::aRuleStopsBeingARuleOnceTheTicksAreEdited()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deeper/one-again.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));

    controller->keepOldest();
    QCOMPARE(controller->ruleText(), QStringLiteral("Keeping the oldest of each group"));

    // One tick changed by hand and the rule no longer describes what will be
    // deleted. Going on saying "keeping the oldest" over ticks somebody has since
    // edited would be the view asserting something untrue.
    controller->toggle(controller->selectedUris().first());
    QCOMPARE(controller->ruleText(), QStringLiteral("Chosen by hand"));

    // And unticking the lot leaves no claim at all rather than a stale one.
    controller->clearSelection();
    QVERIFY(controller->ruleText().isEmpty());
}

void TestDuplicatesTab::keepingOneCopyLeavesEveryOtherGroupAlone()
{
    QVERIFY(startWindow());
    const QByteArray first(4096, 'a');
    const QByteArray second(9000, 'b');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), first));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/two.bin"), second));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/two-copy.bin"), second));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));

    controller->keepNewest();
    const QStringList afterRule = controller->selectedUris();
    QCOMPARE(afterRule.size(), 2);

    // Override one group. That is the whole point of a per-group choice: a rule
    // that is right for every group but one should not have to be abandoned.
    const QString overridden = fixtureRoot(QStringLiteral("pile/deep/one-copy.bin"));
    controller->keepOnly(overridden);

    const QStringList after = controller->selectedUris();
    QVERIFY2(!after.contains(overridden), "the copy chosen to keep is still ticked for removal");
    QVERIFY2(after.contains(fixtureRoot(QStringLiteral("pile/one.bin"))),
        "the other copy in that group was not ticked");
    // The second group is untouched -- whatever the rule decided there still holds.
    const QString otherGroupTick = afterRule.contains(fixtureRoot(QStringLiteral("pile/two.bin")))
        ? fixtureRoot(QStringLiteral("pile/two.bin"))
        : fixtureRoot(QStringLiteral("pile/deep/two-copy.bin"));
    QCOMPARE(after.contains(otherGroupTick), afterRule.contains(otherGroupTick));
    QCOMPARE(controller->ruleText(), QStringLiteral("Chosen by hand"));
}

void TestDuplicatesTab::noPerGroupOverrideEverProposesDeletingEveryCopy()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deeper/one-again.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QCOMPARE(controller->groupCount(), 1);

    // The same rule the four global ones are held to, applied to the new one:
    // something in every group survives. Keeping one copy means ticking the other
    // two, whichever copy is named and however many times it is asked for.
    const QStringList copies { fixtureRoot(QStringLiteral("pile/one.bin")),
        fixtureRoot(QStringLiteral("pile/deep/one-copy.bin")),
        fixtureRoot(QStringLiteral("pile/deeper/one-again.bin")) };
    for (const QString& kept : copies) {
        controller->keepOnly(kept);
        const QStringList ticked = controller->selectedUris();
        QCOMPARE(ticked.size(), 2);
        QVERIFY2(!ticked.contains(kept), qPrintable(QStringLiteral("keeping %1 ticked it").arg(kept)));
    }
}

void TestDuplicatesTab::theChoiceHasWeightOnTheScreen()
{
    QVERIFY(startWindow());
    const QByteArray same(4096, 'a');
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/one.bin"), same));
    QVERIFY(m_harness->writeFile(QStringLiteral("pile/deep/one-copy.bin"), same));

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);

    // Nothing to choose between before a scan, so no panel at all -- it is not a
    // control that should sit there disabled.
    QVERIFY(!shown(QStringLiteral("duplicateKeepPanel")));

    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateKeepPanel")) != nullptr; }));

    // A panel with the weight of the options panel above it, rather than four flat
    // buttons wedged into a strip. Height is the measurement that claim is made of.
    QQuickItem* keep = shown(QStringLiteral("duplicateKeepPanel"));
    QVERIFY2(keep->height() > 60,
        qPrintable(QStringLiteral("the keep panel is %1 pixels tall").arg(keep->height())));
}

void TestDuplicatesTab::everyStrategyNameFitsInThePickerThatOffersIt()
{
    QVERIFY(startWindow());
    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);

    QQuickItem* picker = shown(QStringLiteral("strategyPicker"));
    QVERIFY(picker);
    const QVariantList strategies = controller->strategies();
    QVERIFY(strategies.size() > 1);

    // Every one of them, not only the one showing: the picker is one control and
    // the longest name is the one that decides how wide it has to be. Choosing the
    // strategy is the decision this tab is configured by, and a name cut off
    // mid-word is a decision taken half-blind.
    for (int i = 0; i < strategies.size(); ++i) {
        picker->setProperty("currentIndex", i);
        m_harness->settle();

        auto* content = picker->property("contentItem").value<QQuickItem*>();
        QVERIFY(content);
        const QString label = strategies.at(i).toMap().value(QStringLiteral("label")).toString();
        QCOMPARE(content->property("text").toString(), label);
        QVERIFY2(content->implicitWidth() <= content->width() + 0.5,
            qPrintable(QStringLiteral("the closed picker cuts \"%1\": it needs %2 pixels and has %3")
                           .arg(label)
                           .arg(content->implicitWidth())
                           .arg(content->width())));
    }

    // And in the list it drops down, which is where the choice is actually made.
    // The popup takes its width from the control, and its rows are laid out with
    // padding of their own -- so a name that fits the closed picker to the pixel
    // has less room in the row offering it.
    auto* popup = picker->property("popup").value<QObject*>();
    QVERIFY(popup);
    QMetaObject::invokeMethod(popup, "open");
    QVERIFY(m_harness->until([popup] { return popup->property("opened").toBool(); }));
    m_harness->settle();

    auto* list = popup->property("contentItem").value<QQuickItem*>();
    QVERIFY(list);
    for (const QVariant& strategy : strategies) {
        const QString label = strategy.toMap().value(QStringLiteral("label")).toString();
        QQuickItem* row = itemShowing(list, label);
        QVERIFY2(row, qPrintable(QStringLiteral("no row in the list offers \"%1\"").arg(label)));
        QVERIFY2(row->implicitWidth() <= row->width() + 0.5,
            qPrintable(QStringLiteral("the list cuts \"%1\": it needs %2 pixels and has %3")
                           .arg(label)
                           .arg(row->implicitWidth())
                           .arg(row->width())));
    }
}

// ---- the list is a model, and an arrival is an insertion ----------------
//
// A duplicate scan over a tree with many duplicates in it used to stop the window
// responding, and the list of groups redrew so often that it could not be read or
// scrolled while it filled. The scan was never what was blocked: the groups were
// a QVariantList rebuilt from scratch every time it was read, and it was read once
// per confirmed group -- G² maps and twice as many formatting calls on the drawing
// thread, the last rebuild the largest. A QVariantList also carries no notion of a
// row being added, so every arrival was a wholesale replacement of the view's
// contents. These four assert the shape that fixed it. See MOLE-210.

void TestDuplicatesTab::confirmingEachGroupInsertsOneRowAndResetsNothing()
{
    QVERIFY(writeGroups(3));

    DuplicatesController controller(guiless());
    controller.setStrategyId(QStringLiteral("content"));
    controller.setMinimumSize(1);
    controller.setTargets({ m_tree->rootUri().toString() });

    DuplicateGroupModel* model = controller.groups();
    QVERIFY(model);
    QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);

    controller.scan();
    QVERIFY(waitFor([&] { return !controller.isScanning() && controller.hasRun(); }, 30000));

    QCOMPARE(model->rowCount(), 3);
    // One insertion per confirmed group, and not one reset. The reset is the whole
    // difference: it means a new list, which is every delegate destroyed and built
    // again and the scroll position gone with them.
    QCOMPARE(inserted.count(), 3);
    QCOMPARE(reset.count(), 0);
    // And no row was touched because another one arrived, which is precisely what
    // the rebuild did to every row it had.
    QCOMPARE(changed.count(), 0);
    for (const QList<QVariant>& one : inserted)
        QCOMPARE(one.at(1).toInt(), one.at(2).toInt()); // first == last: one row
}

void TestDuplicatesTab::twoHundredGroupsCostTwoHundredInsertionsAndNothingElse()
{
    // The size at which this was noticed. Two hundred groups meant two hundred
    // rebuilds of a list that was two hundred groups long by the end, so the
    // drawing thread was handed work that grew with the square of the answer.
    const int groups = 200;
    QVERIFY(writeGroups(groups));

    DuplicatesController controller(guiless());
    controller.setStrategyId(QStringLiteral("content"));
    controller.setMinimumSize(1);
    controller.setTargets({ m_tree->rootUri().toString() });

    DuplicateGroupModel* model = controller.groups();
    QVERIFY(model);
    QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);

    controller.scan();
    QVERIFY(waitFor([&] { return !controller.isScanning() && controller.hasRun(); }, 120000));

    QCOMPARE(model->rowCount(), groups);
    // Bounded by the number of groups rather than by their square, which is the
    // same claim MOLE-188 made about a status line reported per entry: what the
    // drawing thread is given has to be bounded by something.
    QCOMPARE(inserted.count(), groups);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 0);

    // The totals are kept as the groups arrive rather than found again by walking
    // all of them, and they are read on every confirmation.
    QCOMPARE(model->copyCount(), groups * 2);
    QVERIFY(model->reclaimableBytes() > 0);
    QVERIFY(controller.summary().contains(QStringLiteral("200 groups")));
}

void TestDuplicatesTab::tickingOneCopyChangesOneRowAndRebuildsNothing()
{
    QVERIFY(writeGroups(3));

    DuplicatesController controller(guiless());
    controller.setStrategyId(QStringLiteral("content"));
    controller.setMinimumSize(1);
    controller.setTargets({ m_tree->rootUri().toString() });
    controller.scan();
    QVERIFY(waitFor([&] { return !controller.isScanning() && controller.hasRun(); }, 30000));

    DuplicateGroupModel* model = controller.groups();
    QCOMPARE(model->rowCount(), 3);

    const QVariantList files = model->data(model->index(1), DuplicateGroupModel::FilesRole).toList();
    QVERIFY(!files.isEmpty());
    const QString ticked = files.first().toMap().value(QStringLiteral("uri")).toString();
    QVERIFY(!ticked.isEmpty());

    QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);

    controller.toggle(ticked);

    // One row, announced once. Ticking a checkbox in a result of five hundred
    // groups used to rebuild and re-create all of them.
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.first().at(0).value<QModelIndex>().row(), 1);
    QCOMPARE(changed.first().at(1).value<QModelIndex>().row(), 1);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(controller.selectedCount(), 1);
    QVERIFY(model->isSelected(ticked));
}

void TestDuplicatesTab::aGroupArrivingLeavesTheScrollPositionWhereItWas()
{
    // Through the real window: the claim is about what the view does with an
    // arrival, and a scroll position is something only a view has.
    QVERIFY(startWindow());
    for (int i = 0; i < 14; ++i) {
        const QByteArray body(1024 + i * 8, static_cast<char>('a' + i % 26));
        QVERIFY(m_harness->writeFile(QStringLiteral("pile/%1/one.bin").arg(i), body));
        QVERIFY(m_harness->writeFile(QStringLiteral("pile/%1/two.bin").arg(i), body));
    }

    DuplicatesController* controller = openTabOn({ fixtureRoot(QStringLiteral("pile")) });
    QVERIFY(controller);
    controller->setStrategyId(QStringLiteral("content"));
    controller->setMinimumSize(1);
    controller->scan();
    QVERIFY(m_harness->until([controller] { return !controller->isScanning() && controller->hasRun(); }));
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("duplicateGroupList")) != nullptr; }));
    m_harness->settle();

    QQuickItem* list = shown(QStringLiteral("duplicateGroupList"));
    QVERIFY(list);
    const qreal reach = list->property("contentHeight").toReal() - list->height();
    QVERIFY2(reach > 0, "the list fits on screen, so there is no scroll position to lose");

    const qreal scrolled = qMin<qreal>(reach, 60);
    list->setProperty("contentY", scrolled);
    m_harness->settle();
    QCOMPARE(list->property("contentY").toReal(), scrolled);

    DuplicateGroupModel* model = controller->groups();
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);

    // Below what is on screen: the rows in view are untouched, so the position is
    // unchanged to the pixel.
    model->insertGroup(madeUpGroup(QStringLiteral("late.bin"), 512), model->rowCount());
    m_harness->settle();
    QCOMPARE(list->property("contentY").toReal(), scrolled);

    // And above it. The rows in view move down the list, so the number is allowed
    // to change to keep them where they are -- what is not allowed is the jump back
    // to the top that a replaced list gave every time.
    model->insertGroup(madeUpGroup(QStringLiteral("earlier.bin"), 4096), 0);
    m_harness->settle();
    QVERIFY2(list->property("contentY").toReal() > 0, "the list jumped back to the top when a group arrived");
    QCOMPARE(reset.count(), 0);
}

// A real window, so a real QGuiApplication rather than the guiless one
// MOLE_TEST_MAIN gives every other controller suite. Material as well, because
// how tall an item is depends on the style the application really runs under,
// and height is what half of these assert on.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestDuplicatesTab testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_DuplicatesTab.moc"
