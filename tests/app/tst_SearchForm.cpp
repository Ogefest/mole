#include "plugins/builtin/SearchFeatures.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
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
    QList<QPair<QString, QString>> pairs {
        { QStringLiteral("Search in"), QStringLiteral("searchScope") },
        { QStringLiteral("Name contains"), QStringLiteral("searchQueryField") },
        { QStringLiteral("Extension"), QStringLiteral("extensionField") },
    };
    if (advanced) {
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

    QVERIFY2(apart.isEmpty(),
        qPrintable(QStringLiteral("%1 of %2 labels are on a different row from their field:\n  %3")
                       .arg(apart.size())
                       .arg(pairs.size())
                       .arg(apart.join(QStringLiteral("\n  ")))));
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
