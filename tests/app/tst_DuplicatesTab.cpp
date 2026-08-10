#include "plugins/builtin/DuplicatesFeature.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

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

private:
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

void TestDuplicatesTab::cleanup()
{
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

MOLE_TEST_MAIN(TestDuplicatesTab)

#include "tst_DuplicatesTab.moc"
