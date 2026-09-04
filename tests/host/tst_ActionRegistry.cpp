#include "host/ActionRegistry.h"
#include "support/MoleTestMain.h"

#include <QVariantMap>

using namespace mole;

namespace {

MenuAction makeAction(
    const QString& id, MenuAction::Section section, const QString& title, int sortOrder = 100)
{
    MenuAction action;
    action.id = id;
    action.section = section;
    action.title = title;
    action.sortOrder = sortOrder;
    action.trigger = [] {};
    return action;
}

QVariantList actionsOf(const QVariantList& sections, const QString& title)
{
    for (const QVariant& entry : sections) {
        const QVariantMap section = entry.toMap();
        if (section.value(QStringLiteral("title")).toString() == title)
            return section.value(QStringLiteral("actions")).toList();
    }
    return {};
}

QStringList titlesOf(const QVariantList& actions)
{
    QStringList out;
    for (const QVariant& entry : actions)
        out.append(entry.toMap().value(QStringLiteral("title")).toString());
    return out;
}

} // namespace

class TestActionRegistry : public QObject
{
    Q_OBJECT

private slots:
    void rejectsIncompleteActions();
    void rejectsDuplicateIds();
    void triggerRunsTheHandler();
    void triggerIgnoresUnknownIds();
    void disabledActionCannotBeTriggered();
    void sectionsComeOutInAFixedOrder();
    void operationsAndWorkflowsAreSeparateSections();
    void emptySectionsAreDropped();
    void actionsAreSortedWithinTheirSection();
    void checkableStateIsEvaluatedEachTime();
    void enabledStateIsEvaluatedEachTime();
    void leadingSeparatorIsSuppressed();
    void pluginsCanSlotBetweenBuiltIns();

    // ---- a handler that changes the registry it is in ----------------------
    void aHandlerThatRemovesItsOwnEntryRunsToTheEnd();
    void aHandlerThatAddsAnEntryLeavesItBehind();
    void aHandlerThatRebuildsAWholePrefixRunsToTheEnd();

    // ---- removing, which nothing covered ----------------------------------
    void removingByIdAndByPrefix();
};

void TestActionRegistry::rejectsIncompleteActions()
{
    ActionRegistry registry;

    MenuAction noId = makeAction(QString(), MenuAction::Section::Workflows, QStringLiteral("x"));
    QVERIFY(!registry.addAction(noId));

    MenuAction noTitle = makeAction(QStringLiteral("a"), MenuAction::Section::Workflows, QString());
    QVERIFY(!registry.addAction(noTitle));

    // An entry with no handler would be a dead menu row.
    MenuAction noHandler;
    noHandler.id = QStringLiteral("b");
    noHandler.title = QStringLiteral("Does nothing");
    QVERIFY(!registry.addAction(noHandler));

    QVERIFY(registry.buildModel().isEmpty());
}

void TestActionRegistry::rejectsDuplicateIds()
{
    ActionRegistry registry;
    QVERIFY(registry.addAction(
        makeAction(QStringLiteral("same"), MenuAction::Section::Workflows, QStringLiteral("First"))));
    QVERIFY(!registry.addAction(
        makeAction(QStringLiteral("same"), MenuAction::Section::Workflows, QStringLiteral("Second"))));

    QCOMPARE(titlesOf(actionsOf(registry.buildModel(), QStringLiteral("Workflows"))),
        QStringList({ QStringLiteral("First") }));
}

void TestActionRegistry::triggerRunsTheHandler()
{
    ActionRegistry registry;
    int calls = 0;

    MenuAction action
        = makeAction(QStringLiteral("run"), MenuAction::Section::Workflows, QStringLiteral("Run"));
    action.trigger = [&calls] { ++calls; };
    QVERIFY(registry.addAction(std::move(action)));

    QVERIFY(registry.trigger(QStringLiteral("run")));
    QCOMPARE(calls, 1);
}

void TestActionRegistry::triggerIgnoresUnknownIds()
{
    ActionRegistry registry;
    QVERIFY(!registry.trigger(QStringLiteral("nope")));
    QVERIFY(!registry.contains(QStringLiteral("nope")));
}

void TestActionRegistry::disabledActionCannotBeTriggered()
{
    ActionRegistry registry;
    int calls = 0;
    bool allowed = false;

    MenuAction action
        = makeAction(QStringLiteral("guarded"), MenuAction::Section::Workflows, QStringLiteral("Guarded"));
    action.trigger = [&calls] { ++calls; };
    action.enabled = [&allowed] { return allowed; };
    registry.addAction(std::move(action));

    // A greyed-out entry is still reachable through a shortcut or a stale
    // click, so the guard has to live here and not only in the view.
    QVERIFY(!registry.trigger(QStringLiteral("guarded")));
    QCOMPARE(calls, 0);

    allowed = true;
    QVERIFY(registry.trigger(QStringLiteral("guarded")));
    QCOMPARE(calls, 1);
}

void TestActionRegistry::sectionsComeOutInAFixedOrder()
{
    ActionRegistry registry;
    // Registered back to front on purpose.
    registry.addAction(makeAction(QStringLiteral("h"), MenuAction::Section::Help, QStringLiteral("H")));
    registry.addAction(makeAction(QStringLiteral("t"), MenuAction::Section::Workflows, QStringLiteral("T")));
    registry.addAction(makeAction(QStringLiteral("v"), MenuAction::Section::View, QStringLiteral("V")));
    registry.addAction(makeAction(QStringLiteral("f"), MenuAction::Section::File, QStringLiteral("F")));

    QStringList sections;
    for (const QVariant& entry : registry.buildModel())
        sections.append(entry.toMap().value(QStringLiteral("title")).toString());

    // Registration order must never reorder the menu, or a newly installed
    // plugin would shuffle the user's muscle memory.
    QCOMPARE(sections,
        QStringList({ QStringLiteral("File"), QStringLiteral("View"), QStringLiteral("Workflows"),
            QStringLiteral("Help") }));
}

void TestActionRegistry::operationsAndWorkflowsAreSeparateSections()
{
    ActionRegistry registry;
    // What used to be one bucket. Doing something to the files in front of you
    // and being handed a tool to work in are different questions, and the menu
    // has to ask them separately -- see ADR-0003.
    registry.addAction(
        makeAction(QStringLiteral("index"), MenuAction::Section::Operations, QStringLiteral("Index this")));
    registry.addAction(
        makeAction(QStringLiteral("rename"), MenuAction::Section::Workflows, QStringLiteral("Bulk rename")));

    const QVariantList model = registry.buildModel();
    QStringList sections;
    for (const QVariant& entry : model)
        sections.append(entry.toMap().value(QStringLiteral("title")).toString());

    // Operations first: the shorter and more frequently wanted list should not
    // have to be read past to reach the other one.
    QCOMPARE(sections, QStringList({ QStringLiteral("Operations"), QStringLiteral("Workflows") }));

    QCOMPARE(actionsOf(model, QStringLiteral("Operations")).size(), 1);
    QCOMPARE(actionsOf(model, QStringLiteral("Operations")).first().toMap().value(QStringLiteral("id")),
        QStringLiteral("index"));
    QCOMPARE(actionsOf(model, QStringLiteral("Workflows")).size(), 1);
    QCOMPARE(actionsOf(model, QStringLiteral("Workflows")).first().toMap().value(QStringLiteral("id")),
        QStringLiteral("rename"));
}

void TestActionRegistry::emptySectionsAreDropped()
{
    ActionRegistry registry;
    registry.addAction(makeAction(QStringLiteral("t"), MenuAction::Section::Workflows, QStringLiteral("T")));

    const QVariantList model = registry.buildModel();
    QCOMPARE(model.size(), 1);
    QCOMPARE(model.first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Workflows"));
}

void TestActionRegistry::actionsAreSortedWithinTheirSection()
{
    ActionRegistry registry;
    registry.addAction(
        makeAction(QStringLiteral("c"), MenuAction::Section::File, QStringLiteral("Third"), 300));
    registry.addAction(
        makeAction(QStringLiteral("a"), MenuAction::Section::File, QStringLiteral("First"), 100));
    registry.addAction(
        makeAction(QStringLiteral("b"), MenuAction::Section::File, QStringLiteral("Second"), 200));

    QCOMPARE(titlesOf(actionsOf(registry.buildModel(), QStringLiteral("File"))),
        QStringList({ QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("Third") }));
}

void TestActionRegistry::checkableStateIsEvaluatedEachTime()
{
    ActionRegistry registry;
    bool on = false;

    MenuAction action
        = makeAction(QStringLiteral("toggle"), MenuAction::Section::View, QStringLiteral("Toggle"));
    action.checked = [&on] { return on; };
    registry.addAction(std::move(action));

    QVariantMap entry = actionsOf(registry.buildModel(), QStringLiteral("View")).first().toMap();
    QVERIFY(entry.value(QStringLiteral("checkable")).toBool());
    QVERIFY(!entry.value(QStringLiteral("checked")).toBool());

    // Rebuilding rather than caching is what keeps the tick honest when the
    // state changed somewhere else entirely.
    on = true;
    entry = actionsOf(registry.buildModel(), QStringLiteral("View")).first().toMap();
    QVERIFY(entry.value(QStringLiteral("checked")).toBool());
}

void TestActionRegistry::enabledStateIsEvaluatedEachTime()
{
    ActionRegistry registry;
    bool available = false;

    MenuAction guarded
        = makeAction(QStringLiteral("guarded"), MenuAction::Section::Workflows, QStringLiteral("Guarded"));
    guarded.enabled = [&available] { return available; };
    registry.addAction(std::move(guarded));

    // No predicate at all means always enabled.
    registry.addAction(
        makeAction(QStringLiteral("plain"), MenuAction::Section::Workflows, QStringLiteral("Plain"), 200));

    QVariantList entries = actionsOf(registry.buildModel(), QStringLiteral("Workflows"));
    QVERIFY(!entries.at(0).toMap().value(QStringLiteral("enabled")).toBool());
    QVERIFY(entries.at(1).toMap().value(QStringLiteral("enabled")).toBool());

    available = true;
    entries = actionsOf(registry.buildModel(), QStringLiteral("Workflows"));
    QVERIFY(entries.at(0).toMap().value(QStringLiteral("enabled")).toBool());
}

void TestActionRegistry::leadingSeparatorIsSuppressed()
{
    ActionRegistry registry;

    MenuAction first
        = makeAction(QStringLiteral("a"), MenuAction::Section::File, QStringLiteral("First"), 100);
    first.separatorBefore = true;
    registry.addAction(std::move(first));

    MenuAction second
        = makeAction(QStringLiteral("b"), MenuAction::Section::File, QStringLiteral("Second"), 200);
    second.separatorBefore = true;
    registry.addAction(std::move(second));

    const QVariantList entries = actionsOf(registry.buildModel(), QStringLiteral("File"));
    // A divider above the very first entry is just a stray line.
    QVERIFY(!entries.at(0).toMap().value(QStringLiteral("separatorBefore")).toBool());
    QVERIFY(entries.at(1).toMap().value(QStringLiteral("separatorBefore")).toBool());
}

void TestActionRegistry::pluginsCanSlotBetweenBuiltIns()
{
    ActionRegistry registry;
    // Built-ins leave gaps in the ordering precisely so this works.
    registry.addAction(makeAction(QStringLiteral("mole.tools.index"), MenuAction::Section::Workflows,
        QStringLiteral("Index this folder"), 10));
    registry.addAction(makeAction(QStringLiteral("mole.tools.late"), MenuAction::Section::Workflows,
        QStringLiteral("Something else"), 100));

    registry.addAction(makeAction(QStringLiteral("org.example.dupes"), MenuAction::Section::Workflows,
        QStringLiteral("Find duplicates"), 50));

    QCOMPARE(titlesOf(actionsOf(registry.buildModel(), QStringLiteral("Workflows"))),
        QStringList({ QStringLiteral("Index this folder"), QStringLiteral("Find duplicates"),
            QStringLiteral("Something else") }));
}

// -------------------- a handler that changes the registry it is in

void TestActionRegistry::aHandlerThatRemovesItsOwnEntryRunsToTheEnd()
{
    // trigger() held a reference into m_actions and invoked the std::function
    // living in it, so an entry whose handler removes it destroyed its own
    // callable while it was on the stack -- along with everything the lambda had
    // captured. "Add current folder" is exactly this shape: Bookmarks::add()
    // emits countChanged, a direct connection rebuilds the bookmark actions, and
    // removeActionsStartingWith() erases the entry being run. Benign today only
    // because of what those particular lambdas capture, and the extension point
    // every plugin's menu entry goes through had no rule about it. See MOLE-365.
    ActionRegistry registry;

    int ranTo = 0;
    // Captured by value, so the capture block is part of the callable being
    // erased -- which is the half a reference-into-the-vector loses.
    const QString marker = QStringLiteral("still here");
    MenuAction action = makeAction(
        QStringLiteral("test.selfremoving"), MenuAction::Section::Workflows, QStringLiteral("Go"));
    action.trigger = [&registry, &ranTo, marker] {
        ranTo = 1;
        registry.removeAction(QStringLiteral("test.selfremoving"));
        // Reading the capture *after* the entry is gone is the whole point.
        if (marker == QStringLiteral("still here"))
            ranTo = 2;
    };
    QVERIFY(registry.addAction(std::move(action)));

    QVERIFY(registry.trigger(QStringLiteral("test.selfremoving")));
    QCOMPARE(ranTo, 2);
    QVERIFY(!registry.contains(QStringLiteral("test.selfremoving")));
}

void TestActionRegistry::aHandlerThatAddsAnEntryLeavesItBehind()
{
    // The other direction, and the one that reallocates: pushing on to the
    // vector while iterating it invalidates the reference the loop is holding.
    ActionRegistry registry;

    bool finished = false;
    MenuAction action
        = makeAction(QStringLiteral("test.adds"), MenuAction::Section::Workflows, QStringLiteral("Add"));
    action.trigger = [&registry, &finished] {
        for (int i = 0; i < 32; ++i) {
            registry.addAction(makeAction(QStringLiteral("test.added.%1").arg(i),
                MenuAction::Section::Workflows, QStringLiteral("Added")));
        }
        finished = true;
    };
    QVERIFY(registry.addAction(std::move(action)));

    QVERIFY(registry.trigger(QStringLiteral("test.adds")));
    QVERIFY(finished);
    QVERIFY(registry.contains(QStringLiteral("test.adds")));
    QVERIFY(registry.contains(QStringLiteral("test.added.31")));
}

void TestActionRegistry::aHandlerThatRebuildsAWholePrefixRunsToTheEnd()
{
    // What picking a bookmark does: the handler opens a place, the tab changes,
    // and the same rebuild wipes every entry under the prefix -- including this
    // one -- while its captures are being read.
    ActionRegistry registry;

    for (int i = 0; i < 4; ++i) {
        registry.addAction(makeAction(QStringLiteral("mole.bookmarks.%1").arg(i),
            MenuAction::Section::Bookmarks, QStringLiteral("Place")));
    }

    int removed = -1;
    QString opened;
    const QString place = QStringLiteral("file:///somewhere");
    MenuAction chosen = makeAction(
        QStringLiteral("mole.bookmarks.chosen"), MenuAction::Section::Bookmarks, QStringLiteral("Go"));
    chosen.trigger = [&registry, &removed, &opened, place] {
        removed = registry.removeActionsStartingWith(QStringLiteral("mole.bookmarks."));
        opened = place; // the captured uri, read after the entry is gone
    };
    QVERIFY(registry.addAction(std::move(chosen)));

    QVERIFY(registry.trigger(QStringLiteral("mole.bookmarks.chosen")));
    QCOMPARE(removed, 5);
    QCOMPARE(opened, place);
    QVERIFY(!registry.contains(QStringLiteral("mole.bookmarks.0")));
}

// ------------------------------ removing, which nothing covered

void TestActionRegistry::removingByIdAndByPrefix()
{
    ActionRegistry registry;
    registry.addAction(makeAction(QStringLiteral("a.one"), MenuAction::Section::File, QStringLiteral("One")));
    registry.addAction(makeAction(QStringLiteral("a.two"), MenuAction::Section::File, QStringLiteral("Two")));
    registry.addAction(
        makeAction(QStringLiteral("b.one"), MenuAction::Section::View, QStringLiteral("Other")));

    QVERIFY(!registry.removeAction(QStringLiteral("nothing.like.this")));
    QVERIFY(registry.removeAction(QStringLiteral("a.one")));
    QVERIFY(!registry.contains(QStringLiteral("a.one")));
    QVERIFY(registry.contains(QStringLiteral("a.two")));

    QCOMPARE(registry.removeActionsStartingWith(QStringLiteral("a.")), 1);
    QCOMPARE(registry.removeActionsStartingWith(QStringLiteral("a.")), 0);
    QVERIFY(registry.contains(QStringLiteral("b.one")));

    // And an id that was removed can be used again, which is what a rebuild
    // depends on.
    QVERIFY(registry.addAction(
        makeAction(QStringLiteral("a.one"), MenuAction::Section::File, QStringLiteral("One again"))));
}

MOLE_TEST_MAIN(TestActionRegistry)
#include "tst_ActionRegistry.moc"
