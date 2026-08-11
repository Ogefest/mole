#include "plugins/builtin/BrowserFeature.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "support/ZipFixtures.h"
#include "ui/AppController.h"
#include "ui/DragSource.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/tasks/TaskManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace mole;
using namespace mole::test;

/// Dragging rows out of a listing and dropping files into one, driven through the
/// real window.
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

    void aDropOfTwoFilesCopiesThemIntoTheFolderInView();
    void thePaneSaysWhatWouldHappenWhileTheDragIsOverIt();
    void aDragThatLeavesWithoutDroppingSaysNothingMore();
    void aReadOnlyPaneDoesNotTakeTheDragAtAll();
    void aCollidingNameOpensTheConfirmationAndWritesNothingUntilItIsAnswered();
    void aDropOnTheInactivePaneMakesItActive();

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

    /// The middle of the listing, which is where a drop lands.
    QPoint overTheListing(const QString& viewName = QStringLiteral("fileList")) const;
    /// A file in a folder that is under no mount at all -- a download folder.
    /// Returns its absolute path.
    QString elsewhere(const QString& name, const QByteArray& contents);
    /// The one visible item with this name. Both panes have one of most things and
    /// one of them is hidden, so asking for "the" item picks whichever came first.
    QQuickItem* visibleItem(const QString& objectName) const;
    static QByteArray contentsOf(const QString& path);

    std::unique_ptr<QmlAppHarness> m_harness;
    /// Where dropped files come from: a directory outside the fixture, so it is
    /// outside every mount the application has -- which is what a download folder
    /// is, and the case VfsManager cannot answer for.
    std::unique_ptr<QTemporaryDir> m_downloads;
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
    m_downloads.reset();
}

QPoint TestDragging::overTheListing(const QString& viewName) const
{
    return m_harness->centreOf(m_harness->item(viewName));
}

QString TestDragging::elsewhere(const QString& name, const QByteArray& contents)
{
    if (!m_downloads)
        m_downloads = std::make_unique<QTemporaryDir>();
    const QString path = QDir(m_downloads->path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(contents);
    return path;
}

QQuickItem* TestDragging::visibleItem(const QString& objectName) const
{
    for (QQuickItem* candidate : m_harness->items(objectName)) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

QByteArray TestDragging::contentsOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
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

// ---- the other direction ---------------------------------------------------
//
// A real drag from another application cannot be driven from here -- it wants a
// platform, a pointer and a nested event loop. What can be driven is everything
// downstream of the events one produces, which is all of Mole's own behaviour:
// the harness builds the payload and delivers the three events by hand.

void TestDragging::aDropOfTwoFilesCopiesThemIntoTheFolderInView()
{
    const QString one = elsewhere(QStringLiteral("invoice.pdf"), QByteArray("%PDF-1.4 invoice"));
    const QString two = elsewhere(QStringLiteral("photo.jpg"), QByteArray("jpeg bytes"));
    QVERIFY(!one.isEmpty() && !two.isEmpty());

    const QPoint where = overTheListing();
    m_harness->dragEnter({ one, two }, where);
    m_harness->dragMove(where);
    m_harness->dropAt(where);

    const QString here = m_harness->fixturePath();
    QVERIFY(m_harness->until([&here] {
        return QFileInfo::exists(QDir(here).filePath(QStringLiteral("invoice.pdf")))
            && QFileInfo::exists(QDir(here).filePath(QStringLiteral("photo.jpg")));
    }));

    // Arrived whole, and the originals are still there: a drop is a copy however
    // the sending application would have preferred it.
    QCOMPARE(contentsOf(QDir(here).filePath(QStringLiteral("invoice.pdf"))), QByteArray("%PDF-1.4 invoice"));
    QVERIFY(QFileInfo::exists(one));
}

void TestDragging::thePaneSaysWhatWouldHappenWhileTheDragIsOverIt()
{
    const QString one = elsewhere(QStringLiteral("one.txt"), QByteArray("1"));
    const QString two = elsewhere(QStringLiteral("two.txt"), QByteArray("2"));

    const QPoint where = overTheListing();
    m_harness->dragEnter({ one, two }, where);
    m_harness->dragMove(where);

    QQuickItem* hint = visibleItem(QStringLiteral("dropHint"));
    QVERIFY2(hint, "the pane says nothing while a drag is over it");

    QQuickItem* text = visibleItem(QStringLiteral("dropHintText"));
    QVERIFY(text);
    const QString said = text->property("text").toString();
    // How many, and into which folder. A count with no destination is the half
    // of the sentence that does not help when two panes are open.
    QVERIFY2(said.contains(QStringLiteral("2 items")), qPrintable(said));
    QVERIFY2(said.contains(pane()->displayPath()), qPrintable(said));

    m_harness->dragLeave();
}

void TestDragging::aDragThatLeavesWithoutDroppingSaysNothingMore()
{
    const QString one = elsewhere(QStringLiteral("one.txt"), QByteArray("1"));

    const QPoint where = overTheListing();
    m_harness->dragEnter({ one }, where);
    m_harness->dragMove(where);
    QVERIFY(visibleItem(QStringLiteral("dropHint")));

    m_harness->dragLeave();

    // The banner goes with the drag. One left behind would be a pane claiming
    // something is about to arrive that never will.
    QVERIFY(!visibleItem(QStringLiteral("dropHint")));
    QVERIFY(!QFileInfo::exists(QDir(m_harness->fixturePath()).filePath(QStringLiteral("one.txt"))));
}

void TestDragging::aReadOnlyPaneDoesNotTakeTheDragAtAll()
{
    // A mounted archive is the read-only drive in Mole, so that is what this uses
    // rather than a wrapper that only declares itself one -- which would test the
    // binding and not the case.
    StoredZip zip;
    zip.add(QByteArrayLiteral("inside/note.txt"), QByteArrayLiteral("a file in an archive"));
    QVERIFY(m_harness->writeFile(QStringLiteral("bundle.zip"), zip.build()));

    const QString archive = m_harness->fixtureUri() + QStringLiteral("/bundle.zip");
    if (!m_harness->app()->isMountableArchive(archive))
        QSKIP("no plugin in this build can mount a zip");

    const QString root = m_harness->app()->openArchive(archive);
    QVERIFY(!root.isEmpty());
    QVERIFY(m_harness->until([this] { return pane() && !pane()->isWritable(); }));
    m_harness->settle(3);

    const QString one = elsewhere(QStringLiteral("one.txt"), QByteArray("1"));
    // The strip keeps a task after it has finished, and mounting the archive was
    // one, so "queued nothing" is a number that did not move.
    const int queued = m_harness->app()->tasks()->rowCount();

    const QPoint where = overTheListing();
    m_harness->dragEnter({ one }, where);
    m_harness->dragMove(where);

    // Not accepted, so the desktop shows it cannot be dropped here -- and nothing
    // is claimed about what would happen, because nothing would.
    QQuickItem* area = visibleItem(QStringLiteral("paneDropArea"));
    QVERIFY(area);
    QVERIFY(!area->property("containsDrag").toBool());
    QVERIFY(!visibleItem(QStringLiteral("dropHint")));

    // And the drop that follows anyway changes nothing.
    m_harness->dropAt(where);
    m_harness->settle(4);
    QCOMPARE(m_harness->app()->tasks()->rowCount(), queued);
}

void TestDragging::aCollidingNameOpensTheConfirmationAndWritesNothingUntilItIsAnswered()
{
    const QString here = m_harness->fixturePath();
    const QString clashing = elsewhere(QStringLiteral("alpha.txt"), QByteArray("the dropped one"));
    QVERIFY(!clashing.isEmpty());
    // alpha.txt is already in the fixture, from init().
    QVERIFY(QFileInfo::exists(QDir(here).filePath(QStringLiteral("alpha.txt"))));

    const QPoint where = overTheListing();
    m_harness->dragEnter({ clashing }, where);
    m_harness->dropAt(where);

    QObject* dialog = m_harness->object(QStringLiteral("transferDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    // The dialog names what clashes, and nothing has been written while it stands.
    const QVariantMap plan = dialog->property("plan").toMap();
    QCOMPARE(
        plan.value(QStringLiteral("collisions")).toStringList(), QStringList { QStringLiteral("alpha.txt") });
    QVERIFY(dialog->property("isDrop").toBool());
    m_harness->settle(4);
    QVERIFY2(contentsOf(QDir(here).filePath(QStringLiteral("alpha.txt"))) != QByteArray("the dropped one"),
        "the file was replaced before anybody agreed to it");

    // Answered with overwrite, which is the answer that has to be asked for.
    QObject* conflict = m_harness->object(QStringLiteral("conflictStrategy"));
    QVERIFY(conflict);
    conflict->setProperty("currentIndex", 2);
    m_harness->settle(2);
    QCOMPARE(conflict->property("currentValue").toString(), QStringLiteral("overwrite"));

    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QVERIFY(m_harness->until([&here] {
        return contentsOf(QDir(here).filePath(QStringLiteral("alpha.txt"))) == QByteArray("the dropped one");
    }));
}

void TestDragging::aDropOnTheInactivePaneMakesItActive()
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(browser);
    browser->setViewMode(BrowserController::ViewMode::Dual);
    m_harness->settle(5);
    QVERIFY(browser->splitEnabled());

    // The right pane starts inactive and shows a different folder, so which pane
    // took the files is a question with an answer.
    browser->setActivePaneIndex(0);
    browser->right()->navigateTo(m_harness->fixtureUri() + QStringLiteral("/epsilon"));
    QVERIFY(m_harness->until([browser] { return !browser->right()->isLoading(); }));
    m_harness->settle(3);
    QCOMPARE(browser->activePaneIndex(), 0);

    QQuickItem* rightListing = nullptr;
    const QList<QQuickItem*> listings = m_harness->items(QStringLiteral("fileList"));
    QVERIFY(listings.size() >= 2);
    rightListing = listings.last();
    QVERIFY(rightListing->isVisible());

    const QString one = elsewhere(QStringLiteral("landed.txt"), QByteArray("over there"));
    const QPoint where = m_harness->centreOf(rightListing);
    m_harness->dragEnter({ one }, where);
    m_harness->dropAt(where);

    // The files are in the right pane's folder now, so that is where the user is.
    QVERIFY(m_harness->until([this] {
        return QFileInfo::exists(
            QDir(m_harness->fixturePath()).filePath(QStringLiteral("epsilon/landed.txt")));
    }));
    QCOMPARE(browser->activePaneIndex(), 1);
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
