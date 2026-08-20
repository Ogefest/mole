#include "plugins/builtin/FileSetsFeature.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/vfs/VfsUri.h"

#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The Sets tab under the keyboard.
///
/// A set's members were reachable only with a mouse: no cursor, so no arrows, no
/// Enter and no F3, and a double click went to the member's *folder* rather than
/// to the member. Everything Mole can do to a file is one key away in a listing
/// and was no keys away here, which made a set something to look at rather than
/// something to work in. See MOLE-205.
class TestSetsTab : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSetOpensWithItsFirstMemberUnderTheCursor();
    void arrowsWalkTheMembers();
    void enterOpensTheMemberNotItsFolder();
    void f3PreviewsTheMemberAndReusesTheOneTab();
    void enterOnAMissingMemberSaysSoAndOpensNothing();
    void theCursorStaysOnTheSameMemberAfterACheck();
    void aDoubleClickOpensTheMemberAndMovesTheCursorToIt();

private:
    /// Builds the window, then a set holding `names` from the window's own
    /// fixture -- the application mounts that one, and a member outside it would
    /// be missing for a reason that is not the one a test is about.
    FileSetsController* setOf(const QStringList& names);
    /// Where the Sets tab is, so a test can come back to it after a key has
    /// opened something else.
    int setsRow() const { return m_setsRow; }
    QString fixtureFile(const QString& name) const;
    QQuickItem* memberList() const { return m_harness->item(QStringLiteral("setMemberList")); }
    /// Where the cursor is, read off the view that owns it. The member list keeps
    /// no cursor of its own -- see FileSetsView.qml.
    int cursorRow() const;

    std::unique_ptr<QmlAppHarness> m_harness;
    int m_setsRow = -1;
};

void TestSetsTab::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
}

void TestSetsTab::cleanup()
{
    m_harness.reset();
    m_setsRow = -1;
}

QString TestSetsTab::fixtureFile(const QString& name) const
{
    return VfsUri::fromString(m_harness->fixtureUri()).child(name).toString();
}

int TestSetsTab::cursorRow() const
{
    QQuickItem* view = m_harness->item(QStringLiteral("setsView"));
    return view ? view->property("cursorRow").toInt() : -2;
}

FileSetsController* TestSetsTab::setOf(const QStringList& names)
{
    for (const QString& name : names) {
        if (!m_harness->writeFile(name, QByteArray(64, 'x')))
            return nullptr;
    }

    m_setsRow = m_harness->app()->tabs()->openTab(QStringLiteral("core.filesets"));
    if (m_setsRow < 0)
        return nullptr;
    auto* sets = qobject_cast<FileSetsController*>(m_harness->app()->tabs()->controllerAt(m_setsRow));
    if (!sets)
        return nullptr;

    if (sets->createSet(QStringLiteral("Reading list")).isEmpty())
        return nullptr;
    QStringList uris;
    for (const QString& name : names)
        uris.append(fixtureFile(name));
    if (sets->addUris(uris) != names.size())
        return nullptr;

    m_harness->settle();
    return sets;
}

void TestSetsTab::aSetOpensWithItsFirstMemberUnderTheCursor()
{
    FileSetsController* sets = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt") });
    QVERIFY(sets);

    QQuickItem* list = memberList();
    QVERIFY2(list, "the member list has to be on screen for a cursor to mean anything");
    QVERIFY2(m_harness->until([this] { return cursorRow() == 0; }),
        "opening a set has to land the cursor on its first member, not on nothing");

    // And the keyboard is here already, without a click. Anything with a
    // selection swallows the keys before the window's fallback sees them, which
    // is the one way this could look right and still not work.
    const QString focus = m_harness->focusChain();
    QVERIFY2(!focus.contains(QStringLiteral("TextField")) && !focus.contains(QStringLiteral("TextInput")),
        qPrintable(QStringLiteral("a text field holds the keyboard: %1").arg(focus)));
}

void TestSetsTab::arrowsWalkTheMembers()
{
    FileSetsController* sets
        = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt"), QStringLiteral("third.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    m_harness->key(Qt::Key_Down);
    QVERIFY2(m_harness->until([this] { return cursorRow() == 1; }), "Down has to move the cursor");
    m_harness->key(Qt::Key_Down);
    QVERIFY(m_harness->until([this] { return cursorRow() == 2; }));

    // Both ends hold. A cursor that walks off the list is a cursor pointing at
    // nothing, and every key after that does nothing for a reason nobody can see.
    m_harness->key(Qt::Key_Down);
    QCOMPARE(cursorRow(), 2);
    m_harness->key(Qt::Key_Up);
    m_harness->key(Qt::Key_Up);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));
    m_harness->key(Qt::Key_Up);
    QCOMPARE(cursorRow(), 0);
}

void TestSetsTab::enterOpensTheMemberNotItsFolder()
{
    FileSetsController* sets = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    m_harness->key(Qt::Key_Down);
    QVERIFY(m_harness->until([this] { return cursorRow() == 1; }));

    m_harness->key(Qt::Key_Return);

    // A browser, with the cursor on the member itself. Opening the folder it is
    // in and leaving the cursor at the top -- which is what the double click did
    // -- makes the person find the file again by hand.
    const QString wanted = fixtureFile(QStringLiteral("second.txt"));
    QVERIFY2(m_harness->until([this, &wanted] {
        auto* browser = m_harness->app()->tabs()->currentController();
        QObject* pane = browser ? browser->property("activePane").value<QObject*>() : nullptr;
        if (!pane)
            return false;
        auto* files = pane->property("files").value<FileListModel*>();
        return files && files->rowOfUri(wanted) == pane->property("currentIndex").toInt()
            && files->rowOfUri(wanted) >= 0;
    }),
        "Enter on a member has to open a browser with the cursor on that member");
}

void TestSetsTab::f3PreviewsTheMemberAndReusesTheOneTab()
{
    FileSetsController* sets = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    const int before = m_harness->app()->tabs()->rowCount();
    m_harness->key(Qt::Key_F3);
    QVERIFY2(m_harness->until([this, before] { return m_harness->app()->tabs()->rowCount() == before + 1; }),
        "F3 has to preview the member under the cursor");
    QCOMPARE(m_harness->app()->tabs()->currentController()->property("currentUri").toString(),
        fixtureFile(QStringLiteral("first.txt")));

    // Back to the set, one member down, and F3 again: the same preview tab, a
    // different file. Twenty members must not leave twenty tabs.
    m_harness->app()->tabs()->setCurrentIndex(setsRow());
    m_harness->settle();
    m_harness->key(Qt::Key_Down);
    QVERIFY(m_harness->until([this] { return cursorRow() == 1; }));
    m_harness->key(Qt::Key_F3);

    QVERIFY(m_harness->until([this] {
        auto* preview = m_harness->app()->tabs()->currentController();
        return preview
            && preview->property("currentUri").toString() == fixtureFile(QStringLiteral("second.txt"));
    }));
    QCOMPARE(m_harness->app()->tabs()->rowCount(), before + 1);
}

void TestSetsTab::enterOnAMissingMemberSaysSoAndOpensNothing()
{
    FileSetsController* sets = setOf({ QStringLiteral("gone.txt"), QStringLiteral("stays.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    QVERIFY(QFile::remove(m_harness->fixturePath() + QStringLiteral("/gone.txt")));
    sets->verify();
    QVERIFY(m_harness->until([sets] { return sets->missingCount() == 1; }));
    m_harness->settle();

    // The cursor is on the member whose file has gone.
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    QSignalSpy said(m_harness->app(), &AppController::notification);
    const int before = m_harness->app()->tabs()->rowCount();
    m_harness->key(Qt::Key_Return);
    m_harness->settle();

    QVERIFY2(said.count() == 1, "Enter on a member whose file has gone has to say so");
    QCOMPARE(m_harness->app()->tabs()->rowCount(), before);
    QCOMPARE(m_harness->app()->tabs()->currentIndex(), setsRow());
}

void TestSetsTab::theCursorStaysOnTheSameMemberAfterACheck()
{
    FileSetsController* sets
        = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt"), QStringLiteral("third.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    m_harness->key(Qt::Key_Down);
    m_harness->key(Qt::Key_Down);
    QVERIFY(m_harness->until([this] { return cursorRow() == 2; }));

    // Checking a set rebuilds the whole member list. A cursor that is a row
    // number would come back at the top, or worse, come to mean another file.
    sets->verify();
    QVERIFY(m_harness->until([sets] { return sets->memberCount() == 3 && sets->missingCount() == 0; }));
    m_harness->settle();
    QCOMPARE(cursorRow(), 2);
}

void TestSetsTab::aDoubleClickOpensTheMemberAndMovesTheCursorToIt()
{
    FileSetsController* sets = setOf({ QStringLiteral("first.txt"), QStringLiteral("second.txt") });
    QVERIFY(sets);
    QVERIFY(m_harness->until([this] { return cursorRow() == 0; }));

    const QList<QQuickItem*> rows = m_harness->items(QStringLiteral("setMemberRow"));
    QCOMPARE(rows.size(), 2);

    // The second row, which is not where the cursor is: a double click has to do
    // what Enter does, on the row it landed on.
    m_harness->doubleClick(m_harness->centreOf(rows.at(1)));

    const QString wanted = fixtureFile(QStringLiteral("second.txt"));
    QVERIFY2(m_harness->until([this, &wanted] {
        auto* browser = m_harness->app()->tabs()->currentController();
        QObject* pane = browser ? browser->property("activePane").value<QObject*>() : nullptr;
        if (!pane)
            return false;
        auto* files = pane->property("files").value<FileListModel*>();
        const int row = files ? files->rowOfUri(wanted) : -1;
        return row >= 0 && row == pane->property("currentIndex").toInt();
    }),
        "a double click has to open the member itself, not the folder it is in");
    QCOMPARE(cursorRow(), 1);
}

// A real window, so a real QGuiApplication and the style the application runs
// under: where the cursor is and whether a key reaches it both depend on it.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestSetsTab testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_SetsTab.moc"
