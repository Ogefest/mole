#include "plugins/builtin/DuplicatesFeature.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
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

    // The assertion, group by group: at least one file is not on the list.
    const QVariantList groups = controller.groups();
    for (const QVariant& value : groups) {
        const QVariantMap group = value.toMap();
        const QVariantList files = group.value(QStringLiteral("files")).toList();
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

    // The body is still the body. It gives up a strip to the Keep row, which only
    // exists once there is something to keep, and nothing else -- the results did
    // not have to grow into space the empty state had been holding back.
    QVERIFY2(bodyHeight() > 200, qPrintable(QStringLiteral("the body is %1 tall").arg(bodyHeight())));
    QVERIFY2(bodyHeight() > beforeScan - 100,
        qPrintable(QStringLiteral("the body fell from %1 to %2").arg(beforeScan).arg(bodyHeight())));

    // And the list fills it, rather than sitting at its natural height with a void
    // underneath -- which is the same fault one state along.
    QQuickItem* list = shown(QStringLiteral("duplicateGroupList"));
    QVERIFY(list);
    QCOMPARE(list->height(), bodyHeight());
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
