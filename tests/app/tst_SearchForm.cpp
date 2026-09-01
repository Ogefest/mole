#include "plugins/builtin/SearchFeatures.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Where the search form's criteria actually land on the screen.
///
/// `LiveSearchView` laid its criteria out in two `GridLayout`s and neither one's
/// rows added up. A `GridLayout` fills cells in order and wraps at `columns`, so a
/// row that uses one cell fewer than the grid has does not leave a gap at the end --
/// it pulls the next item up into it, and everything after that is out by one.
/// Because the deficit accumulated, six labels in the More panel each ended up on a
/// different row from the field they name, and `Extension` in the basic form began a
/// row on its own.
///
/// Asserted by comparing positions rather than by looking, which is the only kind of
/// claim that survives a font change -- and by comparing a label against *its own*
/// field, because "the form looks wrong" is not something a test can hold.
/// See MOLE-270.
class TestSearchForm : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void everyLabelIsOnTheSameRowAsItsField_data();
    void everyLabelIsOnTheSameRowAsItsField();
    void everyCriterionIsReachable_data();
    void everyCriterionIsReachable();
    void theTabOpensWithTheKeyboardInTheLine();
    void typingOnTheLineMovesTheFields();
    void changingAFieldWritesTheLine();
    void scopeIsSaidOnTheLineAsWellAsInTheForm();
    void theCriteriaStandUpOutsideTheSearchView();

private:
    LiveSearchController* openSearch();
    /// Every visible item whose `text` is exactly this.
    QList<QQuickItem*> labelsSaying(const QString& text) const;
    QQuickItem* shown(const QString& objectName) const;
    static void collect(QQuickItem* root, const QString& text, QList<QQuickItem*>& into);
    /// The item's top and bottom in scene coordinates.
    static QPair<qreal, qreal> band(QQuickItem* item);

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestSearchForm::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
}

void TestSearchForm::cleanup()
{
    m_harness.reset();
}

LiveSearchController* TestSearchForm::openSearch()
{
    const int row = m_harness->app()->openFeatureTab(QStringLiteral("mole.livesearch"));
    return row < 0 ? nullptr
                   : qobject_cast<LiveSearchController*>(m_harness->app()->tabs()->controllerAt(row));
}

void TestSearchForm::collect(QQuickItem* root, const QString& text, QList<QQuickItem*>& into)
{
    if (!root)
        return;
    if (root->isVisible() && root->property("text").toString() == text)
        into.append(root);
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children)
        collect(child, text, into);
}

QList<QQuickItem*> TestSearchForm::labelsSaying(const QString& text) const
{
    QList<QQuickItem*> found;
    collect(m_harness->window()->contentItem(), text, found);
    return found;
}

QQuickItem* TestSearchForm::shown(const QString& objectName) const
{
    for (QQuickItem* candidate : m_harness->items(objectName)) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

QPair<qreal, qreal> TestSearchForm::band(QQuickItem* item)
{
    const qreal top = item->mapToScene(QPointF(0, 0)).y();
    return { top, top + item->height() };
}

void TestSearchForm::everyCriterionIsReachable_data()
{
    QTest::addColumn<bool>("withContent");
    QTest::addColumn<bool>("everywhere");

    // The same three states the layout test covers with More open. Which rows are
    // visible changes the panel's height, and its height is the whole question.
    QTest::newRow("More open") << false << false;
    QTest::newRow("More open, searching contents") << true << false;
    QTest::newRow("More open, everywhere indexed") << false << true;
}

void TestSearchForm::everyCriterionIsReachable()
{
    QFETCH(bool, withContent);
    QFETCH(bool, everywhere);

    // There are eleven rows behind More since the name fields joined them, and in
    // a 900-tall window the last of them used to sit *under the task strip*:
    // measured, the grid reached y=880 and y=899 against a strip starting at 860.
    // Nothing in this view scrolled, so the size range and the "Use the index"
    // toggle could not be clicked at all. See MOLE-272.
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    search->setEverywhere(everywhere);
    if (withContent)
        search->setContentText(QStringLiteral("something"));
    m_harness->settle(4);

    QQuickItem* toggle = shown(QStringLiteral("advancedToggle"));
    QVERIFY(toggle);
    QVERIFY(m_harness->clickOn(toggle));
    QVERIFY(m_harness->until([this] { return shown(QStringLiteral("minSizeField")) != nullptr; }));
    m_harness->settle(4);

    QQuickItem* grid = shown(QStringLiteral("advancedCriteria"));
    QVERIFY(grid);
    QQuickItem* strip = shown(QStringLiteral("taskStrip"));
    QVERIFY2(strip, "the strip is what the criteria used to disappear behind");

    // Measured on whatever holds the criteria, so taking the scroll area away
    // fails this with the geometry rather than with a missing object name.
    QQuickItem* area = shown(QStringLiteral("advancedArea"));
    const auto [areaTop, areaBottom] = band(area ? area : grid);
    const auto [stripTop, stripBottom] = band(strip);
    QVERIFY2(areaBottom <= stripTop,
        qPrintable(QStringLiteral("the criteria reach y=%1 and the task strip starts at y=%2, so %3 px of "
                                  "them cannot be clicked")
                       .arg(areaBottom)
                       .arg(stripTop)
                       .arg(areaBottom - stripTop)));

    // And what does not fit is reachable rather than gone. Scrolled to the end,
    // the last criterion has to be inside the viewport -- which is the claim, and
    // is not the same as it having a position somewhere.
    QVERIFY2(area,
        "and they have to be in something that scrolls, or the panel is "
        "only above the strip until somebody adds a criterion");
    const qreal content = area->property("contentHeight").toReal();
    if (content > area->height()) {
        auto* flickable = area->property("contentItem").value<QQuickItem*>();
        QVERIFY(flickable);
        QVERIFY(flickable->setProperty("contentY", content - area->height()));
        m_harness->settle(4);

        QQuickItem* last = shown(QStringLiteral("minSizeField"));
        QVERIFY(last);
        const auto [lastTop, lastBottom] = band(last);
        QVERIFY2(lastTop >= areaTop && lastBottom <= areaBottom + 1,
            qPrintable(QStringLiteral("scrolled to the end, the size field is at %1..%2 and the panel is "
                                      "%3..%4")
                           .arg(lastTop)
                           .arg(lastBottom)
                           .arg(areaTop)
                           .arg(areaBottom)));
    }
}

void TestSearchForm::theTabOpensWithTheKeyboardInTheLine()
{
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    m_harness->settle(6);

    QQuickItem* line = shown(QStringLiteral("queryLineField"));
    QVERIFY2(line, "the line is what the basic view is");
    QVERIFY2(line->hasActiveFocus(),
        "a tab opened with a key has the keyboard in the box it exists for -- and once the form moved "
        "behind More, focusing a field in there would have put it nowhere at all");
}

void TestSearchForm::typingOnTheLineMovesTheFields()
{
    // Typed through the window rather than pushed in through setQueryLine(),
    // because what was broken was exactly the part in between: the widget bound to
    // `controller.queryLine` and assigned to it on every keystroke while no
    // Q_PROPERTY of that name existed, so the read gave undefined and the write
    // went nowhere. Every existing test of the line called the setter in C++ and
    // so could not see it. See ADR-0067.
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    m_harness->settle(6);

    QQuickItem* line = shown(QStringLiteral("queryLineField"));
    QVERIFY(line);
    QVERIFY(line->hasActiveFocus());

    m_harness->type(QStringLiteral("report ext:pdf"));
    QVERIFY(m_harness->until([search] { return search->extension() == QStringLiteral("pdf"); }));
    QCOMPARE(search->queryText(), QStringLiteral("report"));
    // And the line's own widget holds what was typed, which is the half of the
    // round trip a controller assertion cannot see.
    QCOMPARE(line->property("text").toString(), QStringLiteral("report ext:pdf"));
}

void TestSearchForm::changingAFieldWritesTheLine()
{
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    m_harness->settle(6);

    search->setExtension(QStringLiteral("png"));
    m_harness->settle(4);

    QQuickItem* line = shown(QStringLiteral("queryLineField"));
    QVERIFY(line);
    QVERIFY2(line->property("text").toString().contains(QStringLiteral("ext:png")),
        qPrintable(QStringLiteral("the line does not say what the field says: %1")
                       .arg(line->property("text").toString())));
}

void TestSearchForm::scopeIsSaidOnTheLineAsWellAsInTheForm()
{
    // Scope used to be reachable only from the picker in front of More. It is a
    // word now, so both directions have to hold. See ADR-0067.
    LiveSearchController* search = openSearch();
    QVERIFY(search);
    m_harness->settle(6);

    QQuickItem* line = shown(QStringLiteral("queryLineField"));
    QVERIFY(line);
    m_harness->type(QStringLiteral("everywhere:yes"));
    QVERIFY(m_harness->until([search] { return search->everywhere(); }));

    // What the basic view says it is aimed at follows.
    QQuickItem* scope = shown(QStringLiteral("searchScopeText"));
    QVERIFY(scope);
    QVERIFY2(scope->property("text").toString().contains(QStringLiteral("everywhere indexed")),
        qPrintable(scope->property("text").toString()));

    // And back: cleared from the picker's side, the line loses the word.
    search->setEverywhere(false);
    m_harness->settle(4);
    QVERIFY2(!line->property("text").toString().contains(QStringLiteral("everywhere")),
        qPrintable(QStringLiteral("the line still claims a scope the form has dropped: %1")
                       .arg(line->property("text").toString())));
}

void TestSearchForm::everyLabelIsOnTheSameRowAsItsField_data()
{
    QTest::addColumn<bool>("advanced");
    QTest::addColumn<bool>("withContent");
    QTest::addColumn<bool>("everywhere");

    // The conditional rows change which cells are filled, so the claim has to hold
    // with them visible and not: `contentCost` appears once there is text to search
    // for, and `useIndexToggle` disappears when the scope is everywhere indexed.
    QTest::newRow("basic form only") << false << false << false;
    QTest::newRow("More open") << true << false << false;
    QTest::newRow("More open, searching contents") << true << true << false;
    QTest::newRow("More open, everywhere indexed") << true << false << true;
}

void TestSearchForm::everyLabelIsOnTheSameRowAsItsField()
{
    QFETCH(bool, advanced);
    QFETCH(bool, withContent);
    QFETCH(bool, everywhere);

    LiveSearchController* search = openSearch();
    QVERIFY(search);
    search->setEverywhere(everywhere);
    if (withContent)
        search->setContentText(QStringLiteral("something"));
    m_harness->settle(4);

    if (advanced) {
        QQuickItem* toggle = shown(QStringLiteral("advancedToggle"));
        QVERIFY(toggle);
        QVERIFY(m_harness->clickOn(toggle));
        QVERIFY(m_harness->until([this] { return shown(QStringLiteral("minSizeField")) != nullptr; }));
    }
    m_harness->settle(4);

    // Label, and the field it names. Written out rather than derived: which field a
    // label belongs to is the one thing the layout cannot tell us, and it is exactly
    // what was wrong.
    //
    // All of them are behind More since ADR-0067: the basic view has one box that
    // takes a query, and the three that used to sit in front of it -- the scope
    // picker, Name contains and Extension -- moved in here with the rest.
    QList<QPair<QString, QString>> pairs;
    if (advanced) {
        pairs.append({ QStringLiteral("Search in"),
            everywhere ? QStringLiteral("searchVolume") : QStringLiteral("searchRootField") });
        pairs.append({ QStringLiteral("Name contains"), QStringLiteral("searchQueryField") });
        pairs.append({ QStringLiteral("Extension"), QStringLiteral("extensionField") });
        pairs.append({ QStringLiteral("Text inside"), QStringLiteral("contentField") });
        pairs.append({ QStringLiteral("Is a"), QStringLiteral("typeClasses") });
        pairs.append({ QStringLiteral("Changed"), QStringLiteral("modifiedFromField") });
        pairs.append({ QStringLiteral("to"), QStringLiteral("modifiedToField") });
        pairs.append({ QStringLiteral("Path has"), QStringLiteral("pathField") });
        pairs.append({ QStringLiteral("Skip folders"), QStringLiteral("excludedField") });
        pairs.append({ QStringLiteral("Shape"), QStringLiteral("kindMode") });
        pairs.append({ QStringLiteral("Size from"), QStringLiteral("minSizeField") });
        pairs.append({ QStringLiteral("to"), QStringLiteral("maxSizeField") });
    }

    QStringList apart;
    for (const auto& [labelText, fieldName] : pairs) {
        QQuickItem* field = shown(fieldName);
        QVERIFY2(field, qPrintable(QStringLiteral("no visible %1").arg(fieldName)));
        const QList<QQuickItem*> labels = labelsSaying(labelText);
        QVERIFY2(!labels.isEmpty(), qPrintable(QStringLiteral("no label says \"%1\"").arg(labelText)));

        // Overlapping vertically is the claim -- a label and its field are on one row
        // when their bands meet. Not equal tops: a label is shorter than a text field
        // and sits centred in the row.
        const auto [fieldTop, fieldBottom] = band(field);
        bool together = false;
        qreal nearest = -1;
        for (QQuickItem* label : labels) {
            const auto [labelTop, labelBottom] = band(label);
            if (labelTop < fieldBottom && labelBottom > fieldTop) {
                together = true;
                break;
            }
            nearest = labelTop;
        }
        if (!together) {
            apart.append(QStringLiteral("\"%1\" at y=%2 but %3 at y=%4..%5")
                             .arg(labelText)
                             .arg(nearest)
                             .arg(fieldName)
                             .arg(fieldTop)
                             .arg(fieldBottom));
        }
    }

    if (!advanced) {
        // Nothing to pair up, and that is the assertion: the basic view holds the
        // line, what it is aimed at, and no other box that takes a query.
        QVERIFY(pairs.isEmpty());
        QVERIFY2(shown(QStringLiteral("queryLineField")), "the line is the basic view");
        QVERIFY2(shown(QStringLiteral("searchScopeText")), "and it says where it is aimed");
        for (const char* hidden : { "searchQueryField", "extensionField", "nameMode", "searchScope" }) {
            QVERIFY2(!shown(QString::fromLatin1(hidden)),
                qPrintable(QStringLiteral("%1 is still in front of More").arg(QString::fromLatin1(hidden))));
        }
        return;
    }

    QVERIFY2(apart.isEmpty(),
        qPrintable(QStringLiteral("%1 of %2 labels are on a different row from their field:\n  %3")
                       .arg(apart.size())
                       .arg(pairs.size())
                       .arg(apart.join(QStringLiteral("\n  ")))));
}

void TestSearchForm::theCriteriaStandUpOutsideTheSearchView()
{
    // **The assertion that the extraction was one.** `ui/SearchCriteria.qml` was
    // three hundred and sixty-five lines inside LiveSearchView.qml until MOLE-168,
    // and a chain's filter step asks the same questions of the same controller. Two
    // copies of thirty-five fields would have disagreed inside a week, so there is
    // one file with two hosts -- and the only way to know it is a component rather
    // than markup that happens to compile is to build it somewhere else.
    //
    // Here that somewhere is a bare QQmlComponent with nothing around it. The chain
    // editor is MOLE-172's and will be the second host in the application; this is
    // what says the component is ready for it, and what will fail if somebody
    // reaches back into the search view from inside it.
    LiveSearchController* controller = openSearch();
    QVERIFY(controller);
    m_harness->settle(3);

    QQmlEngine* engine = qmlEngine(m_harness->item(QStringLiteral("advancedCriteria")));
    QVERIFY2(engine, "the search view's criteria are not in an engine, so nothing can host them");

    QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/SearchCriteria.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    // The one thing it declares it needs. A `required property` is what makes that a
    // compile-time fact rather than a convention: leave it out and creation fails.
    std::unique_ptr<QObject> alone(component.createWithInitialProperties(
        { { QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject*>(controller)) } }));
    QVERIFY2(alone, qPrintable(component.errorString()));

    auto* asItem = qobject_cast<QQuickItem*>(alone.get());
    QVERIFY2(asItem, "the criteria are not an item, so nothing can lay them out");
    QCOMPARE(asItem->objectName(), QStringLiteral("advancedCriteria"));

    // And its fields are there, bound to the controller it was handed rather than to
    // whatever the search view had: writing one changes the controller's own idea of
    // the query, which is the whole point of hosting it twice.
    QQuickItem* extension = QmlAppHarness::itemIn(asItem, QStringLiteral("extensionField"));
    QVERIFY2(extension, "the extension criterion is missing from the component");
    QVERIFY(extension->setProperty("text", QStringLiteral("csv")));
    QTRY_COMPARE(controller->property("extension").toString(), QStringLiteral("csv"));
}

int main(int argc, char** argv)
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    mole::registerCoreMetaTypes();
    TestSearchForm test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_SearchForm.moc"
