#include "plugins/builtin/IndexesFeature.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/index/IndexDatabase.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The indexes tab with a window behind it.
///
/// `tst_IndexesTab` is headless and covers what the controller answers. This one
/// exists because a fault could not be reached from there: changing a row's
/// *Repeat* dropdown killed the process, and what killed it was the delegate the
/// dropdown lives in, which a headless controller does not have. See MOLE-265.
class TestIndexesView : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void settingARowsScheduleFromTheDropdownDoesNotTakeTheProcessWithIt();
    void choosingFromTheOpenDropdownDoesNotTakeTheProcessWithIt();
    void theScheduleButtonOpensTheOneScheduleTab();

private:
    bool seed(const QString& label, int files);
    IndexesController* openIndexes();
    QQuickItem* shown(const QString& objectName) const;
    static QQuickItem* itemShowing(QQuickItem* root, const QString& text);

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestIndexesView::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
    // The real poll would fire mid-test and make the timing its own variable.
    m_harness->app()->scheduler()->stop();
}

void TestIndexesView::cleanup()
{
    m_harness.reset();
}

bool TestIndexesView::seed(const QString& label, int files)
{
    IndexDatabase* index = m_harness->app()->services().index;
    if (!index)
        return false;
    // Seeding is a write from the guarded thread, on purpose. See MOLE-274.
    UsingTheIndexOnPurpose direct(index);
    const QString uri = m_harness->fixtureUri() + QLatin1Char('/') + label;
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
    if (!index
             ->commitScan(volume.value(), scan.value(), QDateTime::currentDateTime().addSecs(-3 * 86400),
                 ScanOptions {})
             .ok())
        return false;

    // Rows written by hand are written somewhere nothing is looking: the tab
    // reads the interface's snapshot of the index. See ADR-0066.
    return refreshIndexSummary(m_harness->app()->services().indexSummary);
}

QQuickItem* TestIndexesView::itemShowing(QQuickItem* root, const QString& text)
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

IndexesController* TestIndexesView::openIndexes()
{
    const int row = m_harness->app()->openFeatureTab(QStringLiteral("core.indexes"));
    return row < 0 ? nullptr : qobject_cast<IndexesController*>(m_harness->app()->tabs()->controllerAt(row));
}

QQuickItem* TestIndexesView::shown(const QString& objectName) const
{
    for (QQuickItem* candidate : m_harness->items(objectName)) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

/// Changing *Repeat* from never to every day segfaulted.
///
/// `setSchedule()` emits `volumesChanged()`, the list rebuilds on it, and the
/// delegate holding the dropdown is destroyed -- while that dropdown's own
/// `onActivated` handler is still on the stack, because the handler is what called
/// `setSchedule()`. The interpreter then returns into a deleted object.
///
/// Driven through the dropdown's `activated` signal rather than by opening its
/// popup and clicking a row: the popup's own delegates are built by the style and
/// the crash is in the handler, not in the popup. See MOLE-265.
void TestIndexesView::settingARowsScheduleFromTheDropdownDoesNotTakeTheProcessWithIt()
{
    QVERIFY(seed(QStringLiteral("photos"), 4));

    IndexesController* indexes = openIndexes();
    QVERIFY(indexes);
    QVERIFY(m_harness->until([indexes] { return indexes->volumeCount() == 1; }));
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("indexRepeatPicker")) != nullptr; }));

    QQuickItem* picker = shown(QStringLiteral("indexRepeatPicker"));
    QVERIFY(picker);
    QCOMPARE(indexes->scheduledCount(), 0);

    // Index 1 is the first preset after "Repeat: never".
    // A real ComboBox moves its own currentIndex before it emits, and the handler
    // reads  -- so leaving it at 0 would emit for "never" and
    // change nothing.
    QVERIFY(picker->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 1)));
    m_harness->settle(4);

    // Still here, which is the whole claim -- and the schedule actually changed, so
    // a fix that made the handler do nothing would not pass.
    QCOMPARE(indexes->scheduledCount(), 1);
    QVERIFY2(m_harness->app()->scheduler()->store(), "the store outlived the click");
}

/// The same thing the way a person does it: open the dropdown, click a row.
///
/// The case above emits `activated` from C++, which runs the handler but not the
/// rest of what a click does -- the popup closes, and its own delegates go with it,
/// at the same moment the list underneath rebuilds. If the crash needs that, this is
/// the case that finds it. See MOLE-265.
void TestIndexesView::choosingFromTheOpenDropdownDoesNotTakeTheProcessWithIt()
{
    QVERIFY(seed(QStringLiteral("photos"), 4));

    IndexesController* indexes = openIndexes();
    QVERIFY(indexes);
    QVERIFY(m_harness->until([indexes] { return indexes->volumeCount() == 1; }));
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("indexRepeatPicker")) != nullptr; }));

    QQuickItem* picker = shown(QStringLiteral("indexRepeatPicker"));
    QVERIFY(picker);
    QCOMPARE(indexes->scheduledCount(), 0);

    // The dropdown has to hold the keyboard, and this is not a detail: the crash
    // goes through QQuickComboBox::focusOutEvent, which only runs if it had focus to
    // lose. Without this the delegate is still destroyed under the handler and the
    // process survives it, which is why the first three attempts at this test passed
    // against the broken build. See MOLE-265.
    picker->forceActiveFocus();
    QVERIFY(m_harness->until([picker] { return picker->hasActiveFocus(); }));

    auto* popup = picker->property("popup").value<QObject*>();
    QVERIFY(popup);
    QMetaObject::invokeMethod(popup, "open");
    QVERIFY(m_harness->until([popup] { return popup->property("opened").toBool(); }));
    m_harness->settle();

    // Whatever the first preset is called, rather than a wording typed in here.
    const QVariantList presets = indexes->schedulePresets();
    QVERIFY(!presets.isEmpty());
    const QString label = QStringLiteral("Repeat: ")
        + presets.first().toMap().value(QStringLiteral("label")).toString().toLower();

    auto* list = popup->property("contentItem").value<QQuickItem*>();
    QVERIFY(list);
    QQuickItem* row = itemShowing(list, label);
    QVERIFY2(row, qPrintable(QStringLiteral("no row offers \"%1\"").arg(label)));

    QVERIFY(m_harness->clickOn(row));
    m_harness->settle(6);

    QCOMPARE(indexes->scheduledCount(), 1);
}

/// The button that opens the schedule is a route like any other.
///
/// It called `openFeatureTab()` directly, so once the menu entry had been pointed at
/// `openStandingTab()` this button was the one way left to get a second Schedule tab.
/// It now triggers the menu's own action, which means one route rather than two -- and
/// the test that walks the action registry covers it without knowing it exists. This
/// case is the part that registry cannot see: that the button really does go through
/// the action. See MOLE-259.
void TestIndexesView::theScheduleButtonOpensTheOneScheduleTab()
{
    QVERIFY(seed(QStringLiteral("photos"), 4));
    QVERIFY(openIndexes());
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("openAutomationButton")) != nullptr; }));

    const auto scheduleTabs = [this] {
        int found = 0;
        TabsModel* tabs = m_harness->app()->tabs();
        for (int row = 0; row < tabs->rowCount(); ++row) {
            if (tabs->index(row, 0).data(TabsModel::FeatureIdRole).toString()
                == QStringLiteral("core.automation"))
                ++found;
        }
        return found;
    };
    QCOMPARE(scheduleTabs(), 0);

    QVERIFY(m_harness->clickOn(shown(QStringLiteral("openAutomationButton"))));
    QVERIFY(m_harness->until([&scheduleTabs] { return scheduleTabs() == 1; }));

    // Back to the indexes tab, and press it again.
    QVERIFY(openIndexes());
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("openAutomationButton")) != nullptr; }));
    QVERIFY(m_harness->clickOn(shown(QStringLiteral("openAutomationButton"))));
    m_harness->settle(4);
    QCOMPARE(scheduleTabs(), 1);
}

int main(int argc, char** argv)
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    mole::registerCoreMetaTypes();
    TestIndexesView test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_IndexesView.moc"
