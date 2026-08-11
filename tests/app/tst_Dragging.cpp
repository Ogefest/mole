#include "plugins/builtin/BrowserFeature.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/DragSource.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTest>
#include <QUrl>

using namespace mole;
using namespace mole::test;

/// Dragging rows out of a listing, driven through the real window.
///
/// The gesture is the half that cannot be unit tested: a press, a move past
/// whatever the platform calls a threshold, and a payload built from whichever
/// rows that press meant. Everything downstream of `QDrag` is somebody else's
/// program, so the step that hands the payload over is replaced by a recorder --
/// see ADR-0040. Everything upstream of it is Mole's own behaviour and is here.
///
/// Its own binary rather than more of the walkthrough: these tests tick rows and
/// hold a pointer button down across assertions, which is state a narrative test
/// would have to step around.
class TestDragging : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPressAndAMoveHandOverTheRowThatWasPressed();
    void draggingATickedRowSendsTheWholeSelection();
    void draggingAnUntickedRowSendsThatRowAlone();
    void aDragLeavesTheCursorAndTheTickedRowsAlone();
    void aClickStillMovesTheCursor();
    void aDoubleClickStillOpensTheRow();
    void theGridDelegateDragsTheSameWay();

private:
    BrowserPaneController* pane() const;
    FileListModel* files() const;
    /// The delegate showing `row`, from the view itself: `itemAtIndex` is what
    /// knows where a row ended up after layout, scrolling and item reuse.
    QQuickItem* rowItem(int row, const QString& viewName = QStringLiteral("fileList")) const;
    int rowOf(const QString& name) const;
    void tick(const QString& name);
    void switchToGrid();

    /// The names the recorder was handed, in the order they went.
    QStringList sent() const;

    std::unique_ptr<QmlAppHarness> m_harness;
    QList<QUrl> m_urls;
    Qt::DropActions m_actions = Qt::IgnoreAction;
    int m_handovers = 0;
};

void TestDragging::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));

    QVERIFY(m_harness->writeFile(QStringLiteral("alpha.txt")));
    QVERIFY(m_harness->writeFile(QStringLiteral("beta.txt")));
    QVERIFY(m_harness->writeFile(QStringLiteral("gamma.txt")));
    QVERIFY(m_harness->writeFile(QStringLiteral("delta.txt")));
    QVERIFY(m_harness->makeDirs(QStringLiteral("epsilon")));
    QVERIFY(m_harness->writeFile(QStringLiteral("epsilon/inside.txt")));

    m_urls.clear();
    m_actions = Qt::IgnoreAction;
    m_handovers = 0;

    DragSource* source = m_harness->app()->dragSource();
    QVERIFY(source);
    source->setStartHook([this](std::unique_ptr<QMimeData> mime, Qt::DropActions actions) {
        ++m_handovers;
        m_urls = mime->urls();
        m_actions = actions;
        return true;
    });

    pane()->refresh();
    QVERIFY(m_harness->until([this] { return files()->rowCount() >= 5; }));
    m_harness->settle(3);
}

void TestDragging::cleanup()
{
    m_harness.reset();
}

BrowserPaneController* TestDragging::pane() const
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    return browser ? browser->activePane() : nullptr;
}

FileListModel* TestDragging::files() const
{
    return pane() ? pane()->files() : nullptr;
}

QQuickItem* TestDragging::rowItem(int row, const QString& viewName) const
{
    QQuickItem* view = m_harness->item(viewName);
    if (!view)
        return nullptr;
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, row));
    return item;
}

int TestDragging::rowOf(const QString& name) const
{
    for (int row = 0; row < files()->rowCount(); ++row) {
        if (files()->nameAt(row) == name)
            return row;
    }
    return -1;
}

void TestDragging::tick(const QString& name)
{
    const int row = rowOf(name);
    if (row >= 0)
        files()->setSelected(row, true);
}

void TestDragging::switchToGrid()
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(browser);
    browser->setViewMode(BrowserController::ViewMode::Grid);
    m_harness->settle(4);
}

QStringList TestDragging::sent() const
{
    QStringList names;
    for (const QUrl& url : m_urls)
        names.append(url.fileName());
    return names;
}

void TestDragging::aPressAndAMoveHandOverTheRowThatWasPressed()
{
    const int row = rowOf(QStringLiteral("beta.txt"));
    QVERIFY(row >= 0);
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->dragFrom(m_harness->centreOf(item));

    // Nothing ticked, so the press is the whole answer.
    QCOMPARE(m_handovers, 1);
    QCOMPARE(sent(), QStringList { QStringLiteral("beta.txt") });
    // A drag out of Mole is a copy, whatever the receiver would prefer.
    QCOMPARE(m_actions, Qt::DropActions(Qt::CopyAction));
    QVERIFY(m_urls.first().isLocalFile());

    m_harness->release(m_harness->centreOf(item));
}

void TestDragging::draggingATickedRowSendsTheWholeSelection()
{
    tick(QStringLiteral("alpha.txt"));
    tick(QStringLiteral("beta.txt"));
    tick(QStringLiteral("gamma.txt"));
    m_harness->settle(2);
    QCOMPARE(files()->selectionCount(), 3);

    // Any of the three, not just the first: the rule is about membership.
    const int row = rowOf(QStringLiteral("gamma.txt"));
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->dragFrom(m_harness->centreOf(item));

    QCOMPARE(m_handovers, 1);
    QStringList names = sent();
    names.sort();
    QCOMPARE(names,
        QStringList(
            { QStringLiteral("alpha.txt"), QStringLiteral("beta.txt"), QStringLiteral("gamma.txt") }));

    m_harness->release(m_harness->centreOf(item));
}

void TestDragging::draggingAnUntickedRowSendsThatRowAlone()
{
    tick(QStringLiteral("alpha.txt"));
    tick(QStringLiteral("beta.txt"));
    tick(QStringLiteral("gamma.txt"));
    m_harness->settle(2);

    // A fourth row, outside the selection. Sending the ticked three here would be
    // the pointer answering a question about the keyboard's cursor.
    const int row = rowOf(QStringLiteral("delta.txt"));
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->dragFrom(m_harness->centreOf(item));

    QCOMPARE(m_handovers, 1);
    QCOMPARE(sent(), QStringList { QStringLiteral("delta.txt") });

    m_harness->release(m_harness->centreOf(item));
}

void TestDragging::aDragLeavesTheCursorAndTheTickedRowsAlone()
{
    tick(QStringLiteral("alpha.txt"));
    tick(QStringLiteral("beta.txt"));
    m_harness->settle(2);
    pane()->setCurrentIndex(0);
    m_harness->settle(2);

    const int before = pane()->currentIndex();
    const QStringList ticked = files()->selectedUris();

    const int row = rowOf(QStringLiteral("delta.txt"));
    QVERIFY(row != before);
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->dragFrom(m_harness->centreOf(item));
    m_harness->release(m_harness->centreOf(item));

    // Dragging is not selecting. Somebody who drags one file out of a ticked set
    // has to find the set exactly as they left it -- and the cursor too, or the
    // next Enter opens something they never pointed at.
    QCOMPARE(m_handovers, 1);
    QCOMPARE(pane()->currentIndex(), before);
    QCOMPARE(files()->selectedUris(), ticked);
}

void TestDragging::aClickStillMovesTheCursor()
{
    pane()->setCurrentIndex(0);
    m_harness->settle(2);

    const int row = rowOf(QStringLiteral("gamma.txt"));
    QVERIFY(row > 0);
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->click(m_harness->centreOf(item));

    // The handler must not eat the press it did not use.
    QCOMPARE(pane()->currentIndex(), row);
    QCOMPARE(m_handovers, 0);
}

void TestDragging::aDoubleClickStillOpensTheRow()
{
    const QString before = pane()->currentUri();
    const int row = rowOf(QStringLiteral("epsilon"));
    QVERIFY(row >= 0);
    QQuickItem* item = rowItem(row);
    QVERIFY(item);

    m_harness->doubleClick(m_harness->centreOf(item));

    QVERIFY(m_harness->until([this, before] { return pane()->currentUri() != before; }));
    QVERIFY(pane()->currentUri().endsWith(QStringLiteral("/epsilon")));
    QCOMPARE(m_handovers, 0);
}

void TestDragging::theGridDelegateDragsTheSameWay()
{
    switchToGrid();
    QQuickItem* grid = m_harness->item(QStringLiteral("fileGrid"));
    QVERIFY(grid);
    QVERIFY(grid->isVisible());

    // One row alone, from a tile.
    int row = rowOf(QStringLiteral("beta.txt"));
    QQuickItem* tile = rowItem(row, QStringLiteral("fileGrid"));
    QVERIFY(tile);
    m_harness->dragFrom(m_harness->centreOf(tile));
    QCOMPARE(m_handovers, 1);
    QCOMPARE(sent(), QStringList { QStringLiteral("beta.txt") });
    m_harness->release(m_harness->centreOf(tile));

    // And the selection, from a ticked tile.
    tick(QStringLiteral("alpha.txt"));
    tick(QStringLiteral("beta.txt"));
    m_harness->settle(2);
    row = rowOf(QStringLiteral("alpha.txt"));
    tile = rowItem(row, QStringLiteral("fileGrid"));
    QVERIFY(tile);
    m_harness->dragFrom(m_harness->centreOf(tile));
    QCOMPARE(m_handovers, 2);
    QStringList names = sent();
    names.sort();
    QCOMPARE(names, QStringList({ QStringLiteral("alpha.txt"), QStringLiteral("beta.txt") }));
    m_harness->release(m_harness->centreOf(tile));

    // A click on a tile still moves the cursor, as it does in the list.
    const int other = rowOf(QStringLiteral("gamma.txt"));
    QQuickItem* otherTile = rowItem(other, QStringLiteral("fileGrid"));
    QVERIFY(otherTile);
    m_harness->click(m_harness->centreOf(otherTile));
    QCOMPARE(pane()->currentIndex(), other);
    QCOMPARE(m_handovers, 2);
}

// A real window, so a real QGuiApplication rather than the guiless one every
// other suite gets from MOLE_TEST_MAIN. Material as well, because a pointer that
// lands on a row depends on how tall the style makes one.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestDragging testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Dragging.moc"
