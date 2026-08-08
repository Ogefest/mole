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
    void emptySectionsAreDropped();
    void actionsAreSortedWithinTheirSection();
    void checkableStateIsEvaluatedEachTime();
    void enabledStateIsEvaluatedEachTime();
    void leadingSeparatorIsSuppressed();
    void pluginsCanSlotBetweenBuiltIns();
};

void TestActionRegistry::rejectsIncompleteActions()
{
    ActionRegistry registry;

    MenuAction noId = makeAction(QString(), MenuAction::Section::Tools, QStringLiteral("x"));
    QVERIFY(!registry.addAction(noId));

    MenuAction noTitle = makeAction(QStringLiteral("a"), MenuAction::Section::Tools, QString());
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
        makeAction(QStringLiteral("same"), MenuAction::Section::Tools, QStringLiteral("First"))));
    QVERIFY(!registry.addAction(
        makeAction(QStringLiteral("same"), MenuAction::Section::Tools, QStringLiteral("Second"))));

    QCOMPARE(titlesOf(actionsOf(registry.buildModel(), QStringLiteral("Tools"))),
        QStringList({ QStringLiteral("First") }));
}

void TestActionRegistry::triggerRunsTheHandler()
{
    ActionRegistry registry;
    int calls = 0;

    MenuAction action = makeAction(QStringLiteral("run"), MenuAction::Section::Tools, QStringLiteral("Run"));
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
        = makeAction(QStringLiteral("guarded"), MenuAction::Section::Tools, QStringLiteral("Guarded"));
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
    registry.addAction(makeAction(QStringLiteral("t"), MenuAction::Section::Tools, QStringLiteral("T")));
    registry.addAction(makeAction(QStringLiteral("v"), MenuAction::Section::View, QStringLiteral("V")));
    registry.addAction(makeAction(QStringLiteral("f"), MenuAction::Section::File, QStringLiteral("F")));

    QStringList sections;
    for (const QVariant& entry : registry.buildModel())
        sections.append(entry.toMap().value(QStringLiteral("title")).toString());

    // Registration order must never reorder the menu, or a newly installed
    // plugin would shuffle the user's muscle memory.
    QCOMPARE(sections,
        QStringList({ QStringLiteral("File"), QStringLiteral("View"), QStringLiteral("Tools"),
            QStringLiteral("Help") }));
}

void TestActionRegistry::emptySectionsAreDropped()
{
    ActionRegistry registry;
    registry.addAction(makeAction(QStringLiteral("t"), MenuAction::Section::Tools, QStringLiteral("T")));

    const QVariantList model = registry.buildModel();
    QCOMPARE(model.size(), 1);
    QCOMPARE(model.first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Tools"));
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
        = makeAction(QStringLiteral("guarded"), MenuAction::Section::Tools, QStringLiteral("Guarded"));
    guarded.enabled = [&available] { return available; };
    registry.addAction(std::move(guarded));

    // No predicate at all means always enabled.
    registry.addAction(
        makeAction(QStringLiteral("plain"), MenuAction::Section::Tools, QStringLiteral("Plain"), 200));

    QVariantList entries = actionsOf(registry.buildModel(), QStringLiteral("Tools"));
    QVERIFY(!entries.at(0).toMap().value(QStringLiteral("enabled")).toBool());
    QVERIFY(entries.at(1).toMap().value(QStringLiteral("enabled")).toBool());

    available = true;
    entries = actionsOf(registry.buildModel(), QStringLiteral("Tools"));
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
    registry.addAction(makeAction(QStringLiteral("mole.tools.index"), MenuAction::Section::Tools,
        QStringLiteral("Index this folder"), 10));
    registry.addAction(makeAction(
        QStringLiteral("mole.tools.late"), MenuAction::Section::Tools, QStringLiteral("Something else"), 100));

    registry.addAction(makeAction(QStringLiteral("org.example.dupes"), MenuAction::Section::Tools,
        QStringLiteral("Find duplicates"), 50));

    QCOMPARE(titlesOf(actionsOf(registry.buildModel(), QStringLiteral("Tools"))),
        QStringList({ QStringLiteral("Index this folder"), QStringLiteral("Find duplicates"),
            QStringLiteral("Something else") }));
}

MOLE_TEST_MAIN(TestActionRegistry)
#include "tst_ActionRegistry.moc"
