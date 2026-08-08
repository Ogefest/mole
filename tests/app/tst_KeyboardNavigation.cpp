#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/vfs/VfsUri.h"

#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Drives the real QML through real key events.
///
/// The model-level tests already prove the controllers move a cursor; what
/// they cannot prove is that a keystroke ever reaches them. That gap is where
/// the arrow keys quietly stopped working, so this suite exists to close it.
class TestKeyboardNavigation : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void focusSurvivesWindowReactivation();
    void downArrowMovesTheCursor();
    void upArrowMovesBack();
    void cursorStopsAtBothEnds();
    void homeAndEndJump();
    void enterOpensTheFolderUnderTheCursor();
    void backspaceGoesUp();
    void insertTicksAndAdvances();
    void pathBarOwnsTheKeyboardWhileEditing();
    void typingInThePathBarStillWorks();
    void enterWorksEvenWhenARowHoldsFocus();
    void listHasFocusOnStartup();
    void enterStillOpensFoldersAfterReactivation();
    void switchingTabsPutsTheKeyboardBackOnTheList();
    void enterWorksWhenFocusIsOutsideThePane();
    void typingIsNeverStolenByTheFallback();
    void searchTabStartsWhereTheUserIs();
    void ctrlGFocusesThePathBar();
    void cursorStaysInStepAfterAClick();
    void enterOpensTheRightRowAfterNavigating();
    void ctrlArrowsNavigateHistory();
    void deleteAsksWithTheFilesNamed();
    void typingStartsFilteringWithoutAShortcut();
    void modifiedKeysDoNotStartFiltering();
    void f3OpensAPreviewAndReusesTheTab();
    void f3OnAFolderOpensItInstead();
    void previewArrowsStepThroughTheFolder();
    void newTabShortcutOpensATab();
    void f4MenuWalksIntoSubmenusWithTheKeyboard();

private:
    BrowserPaneController* pane() const;
    /// QObject::findChild does not follow the visual tree that Loader and
    /// SplitView build, so walk the item tree instead.
    static QQuickItem* findItem(QQuickItem* root, const QString& objectName);
    QQuickItem* findItem(const QString& objectName) const;
    /// A Menu is a Popup, not an Item, so it never appears in the visual tree
    /// that findItem walks.
    QObject* findObject(const QString& objectName) const;
    void pressKey(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void settle();

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow* m_window = nullptr;
};

void TestKeyboardNavigation::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestKeyboardNavigation::init()
{
    // Each test gets a fresh profile: otherwise the session written by the
    // previous one would restore tabs pointing at a temporary tree that has
    // since been deleted.
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    // Directories sort first, so rows 0..2 are the folders in name order.
    QVERIFY(m_tree->makeDirs(QStringLiteral("alpha")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("beta")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("gamma")));
    QVERIFY(m_tree->writeFile(QStringLiteral("alpha/inside.txt")));
    QVERIFY(m_tree->writeFile(QStringLiteral("one.txt")));
    QVERIFY(m_tree->writeFile(QStringLiteral("two.txt")));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));

    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("App"), m_app.get());
    m_engine->load(QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/Main.qml")));

    QVERIFY2(!m_engine->rootObjects().isEmpty(), "the user interface failed to load");
    m_window = qobject_cast<QQuickWindow*>(m_engine->rootObjects().first());
    QVERIFY(m_window);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));

    // Wait for the initial listing so the cursor has rows to move over.
    QVERIFY(waitFor([this] { return pane() && pane()->files()->rowCount() == 5; }, 10000));
    settle();
}

void TestKeyboardNavigation::cleanup()
{
    m_engine.reset();
    m_window = nullptr;
    m_app.reset();
    m_tree.reset();
}

BrowserPaneController* TestKeyboardNavigation::pane() const
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->currentController());
    return browser ? browser->activePane() : nullptr;
}

QQuickItem* TestKeyboardNavigation::findItem(QQuickItem* root, const QString& objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = findItem(child, objectName))
            return found;
    }
    return nullptr;
}

QQuickItem* TestKeyboardNavigation::findItem(const QString& objectName) const
{
    return findItem(m_window->contentItem(), objectName);
}

void TestKeyboardNavigation::settle()
{
    // Let bindings, queued task results and the render loop catch up.
    for (int i = 0; i < 5; ++i)
        QTest::qWait(20);
}

void TestKeyboardNavigation::pressKey(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(m_window, key, modifiers);
    settle();
}

void TestKeyboardNavigation::focusSurvivesWindowReactivation()
{
    QVERIFY(m_window->activeFocusItem() != nullptr);

    // Alt-tabbing away and back must not leave the keyboard dead.
    m_window->setVisible(false);
    QTest::qWait(50);
    m_window->setVisible(true);
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    settle();

    const int before = pane()->currentIndex();
    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), before + 1);
}

void TestKeyboardNavigation::downArrowMovesTheCursor()
{
    QCOMPARE(pane()->currentIndex(), 0);

    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), 1);

    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), 2);
}

void TestKeyboardNavigation::upArrowMovesBack()
{
    pressKey(Qt::Key_Down);
    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), 2);

    pressKey(Qt::Key_Up);
    QCOMPARE(pane()->currentIndex(), 1);
}

void TestKeyboardNavigation::cursorStopsAtBothEnds()
{
    pressKey(Qt::Key_Up);
    QCOMPARE(pane()->currentIndex(), 0);

    for (int i = 0; i < 10; ++i)
        QTest::keyClick(m_window, Qt::Key_Down);
    settle();

    // Five entries, so the last row is 4 and it must not run off the end.
    QCOMPARE(pane()->currentIndex(), 4);
}

void TestKeyboardNavigation::homeAndEndJump()
{
    pressKey(Qt::Key_End);
    QCOMPARE(pane()->currentIndex(), 4);

    pressKey(Qt::Key_Home);
    QCOMPARE(pane()->currentIndex(), 0);
}

void TestKeyboardNavigation::enterOpensTheFolderUnderTheCursor()
{
    // Row 0 is "alpha", the first directory.
    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("alpha"));

    pressKey(Qt::Key_Return);
    QVERIFY(waitFor(
        [this] {
            return pane()->currentUri() == m_tree->rootUri().child(QStringLiteral("alpha")).toString();
        },
        10000));

    QVERIFY(waitFor([this] { return pane()->files()->rowCount() == 1; }));
    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("inside.txt"));
}

void TestKeyboardNavigation::backspaceGoesUp()
{
    pressKey(Qt::Key_Return); // into alpha/
    QVERIFY(waitFor([this] { return pane()->files()->rowCount() == 1; }, 10000));

    pressKey(Qt::Key_Backspace);
    QVERIFY(waitFor([this] { return pane()->currentUri() == m_tree->rootUri().toString(); }, 10000));
}

void TestKeyboardNavigation::insertTicksAndAdvances()
{
    pressKey(Qt::Key_Insert);
    QCOMPARE(pane()->files()->selectionCount(), 1);
    QCOMPARE(pane()->currentIndex(), 1);

    pressKey(Qt::Key_Insert);
    QCOMPARE(pane()->files()->selectionCount(), 2);
    QCOMPARE(pane()->currentIndex(), 2);
}

void TestKeyboardNavigation::pathBarOwnsTheKeyboardWhileEditing()
{
    // Typing a destination means the path bar has the keyboard and nothing
    // else may act on it. Enter there navigates to what was typed -- it must
    // not also open whatever row the cursor happens to sit on, which is what
    // made the Ctrl+G flow open files nobody asked for.
    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    pathField->forceActiveFocus();
    settle();
    QVERIFY(pathField->hasActiveFocus());

    const QString before = pane()->currentUri();
    const int cursorBefore = pane()->currentIndex();

    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), cursorBefore);

    pressKey(Qt::Key_Return);
    settle();
    QCOMPARE(pane()->currentUri(), before);

    // Escape hands the keyboard back and the list responds again.
    pressKey(Qt::Key_Escape);
    QVERIFY(!pathField->hasActiveFocus());
    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), cursorBefore + 1);
}

void TestKeyboardNavigation::typingInThePathBarStillWorks()
{
    // The other half of the same rule: keys that a focused text field really
    // does want must not be stolen from it.
    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    pathField->forceActiveFocus();
    settle();

    const QString before = pathField->property("text").toString();
    QTest::keyClick(m_window, Qt::Key_X);
    QTest::keyClick(m_window, Qt::Key_Y);
    settle();

    QCOMPARE(pathField->property("text").toString(), before + QStringLiteral("xy"));
}

void TestKeyboardNavigation::enterWorksEvenWhenARowHoldsFocus()
{
    // ListView gives the keyboard to its current delegate, and ItemDelegate is
    // a button underneath -- a focused button swallows Return. That is exactly
    // what stopped Enter opening folders while every other key kept working.
    QQuickItem* focused = m_window->activeFocusItem();
    QVERIFY(focused);
    QVERIFY2(QString::fromLatin1(focused->metaObject()->className()).contains(QStringLiteral("ItemDelegate")),
        "the view is expected to focus its current row; the fix makes that harmless");

    pressKey(Qt::Key_Return);
    QVERIFY2(waitFor(
                 [this] {
                     return pane()->currentUri()
                         == m_tree->rootUri().child(QStringLiteral("alpha")).toString();
                 },
                 5000),
        "Enter opens the folder under the cursor");
}

void TestKeyboardNavigation::listHasFocusOnStartup()
{
    // The path field is the first focusable control in the pane, so without a
    // deliberate default the window manager hands it the keyboard and Enter
    // ends up re-submitting the path instead of opening the folder.
    QQuickItem* focused = m_window->activeFocusItem();
    QVERIFY(focused);

    QStringList chain;
    for (QQuickItem* item = focused; item; item = item->parentItem()) {
        QString name = QString::fromLatin1(item->metaObject()->className());
        name.remove(QRegularExpression(QStringLiteral("_QMLTYPE_.*")));
        if (!item->objectName().isEmpty())
            name += QStringLiteral("[%1]").arg(item->objectName());
        chain.prepend(name);
    }
    qInfo().noquote() << "focus chain:" << chain.join(QStringLiteral(" > "));

    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    QVERIFY2(!pathField->hasActiveFocus(), "the file list, not the path bar, owns the keyboard");
}

void TestKeyboardNavigation::enterStillOpensFoldersAfterReactivation()
{
    // Alt-tabbing away and back is when focus quietly moves somewhere else.
    m_window->setVisible(false);
    QTest::qWait(50);
    m_window->setVisible(true);
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    settle();

    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("alpha"));
    pressKey(Qt::Key_Return);

    QVERIFY2(waitFor(
                 [this] {
                     return pane()->currentUri()
                         == m_tree->rootUri().child(QStringLiteral("alpha")).toString();
                 },
                 5000),
        "Enter must still open the folder under the cursor");
}

void TestKeyboardNavigation::switchingTabsPutsTheKeyboardBackOnTheList()
{
    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    pathField->forceActiveFocus();
    settle();
    QVERIFY(pathField->hasActiveFocus());

    // Leaving the tab and coming back has to reset the keyboard to the list,
    // or Enter stays swallowed by the path bar for the rest of the session.
    const int second = m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    QVERIFY(second >= 0);
    settle();
    m_app->tabs()->setCurrentIndex(0);
    settle();
    settle();

    QVERIFY2(!pathField->hasActiveFocus(), "focus must return to the list");

    pressKey(Qt::Key_Return);
    QVERIFY2(waitFor(
                 [this] {
                     return pane()->currentUri()
                         == m_tree->rootUri().child(QStringLiteral("alpha")).toString();
                 },
                 5000),
        "Enter must open the folder once focus is back on the list");
}

void TestKeyboardNavigation::enterWorksWhenFocusIsOutsideThePane()
{
    // Focus parked on the sidebar, which is nowhere near the file list.
    //
    // Arrows are deliberately left alone here: a focused list drives itself,
    // and taking that away would be wrong. Enter is the one that matters --
    // the sidebar wants nothing to do with it, so the window catches it.
    QQuickItem* drives = findItem(QStringLiteral("driveList"));
    QVERIFY(drives);
    drives->forceActiveFocus();
    settle();

    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("alpha"));
    pressKey(Qt::Key_Return);
    QVERIFY2(waitFor(
                 [this] {
                     return pane()->currentUri()
                         == m_tree->rootUri().child(QStringLiteral("alpha")).toString();
                 },
                 5000),
        "Enter must open the folder even when focus sits elsewhere");

    pressKey(Qt::Key_Backspace);
    QVERIFY(waitFor([this] { return pane()->currentUri() == m_tree->rootUri().toString(); }, 5000));
}

void TestKeyboardNavigation::typingIsNeverStolenByTheFallback()
{
    // The other half of the rule: a text input keeps every key it wants,
    // including Enter and the arrows.
    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    pathField->forceActiveFocus();
    settle();

    const QString before = pathField->property("text").toString();
    QTest::keyClick(m_window, Qt::Key_Z);
    settle();
    QCOMPARE(pathField->property("text").toString(), before + QStringLiteral("z"));
}

void TestKeyboardNavigation::searchTabStartsWhereTheUserIs()
{
    // Walk somewhere first, then ask for a search.
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->currentController());
    QVERIFY(browser);
    const QString target = m_tree->rootUri().child(QStringLiteral("alpha")).toString();
    browser->navigateActive(target);
    QVERIFY(waitFor([browser] { return !browser->activePane()->isLoading(); }));

    const int row = m_app->openFeatureTab(QStringLiteral("mole.livesearch"));
    QVERIFY(row >= 0);

    QObject* search = m_app->tabs()->controllerAt(row);
    QVERIFY(search);
    QCOMPARE(search->property("rootUri").toString(), target);
}

void TestKeyboardNavigation::ctrlGFocusesThePathBar()
{
    QQuickItem* pathField = findItem(QStringLiteral("pathField"));
    QVERIFY(pathField);
    QVERIFY(!pathField->hasActiveFocus());

    pressKey(Qt::Key_G, Qt::ControlModifier);
    QVERIFY2(pathField->hasActiveFocus(), "Ctrl+G must put the cursor in the path bar");
    // Selected, so typing replaces rather than appends to the current path.
    QVERIFY(!pathField->property("selectedText").toString().isEmpty());
}

void TestKeyboardNavigation::cursorStaysInStepAfterAClick()
{
    // Clicking used to assign the view's currentIndex directly, which breaks
    // the QML binding to the controller for good. Everything after that came
    // out of step.
    QQuickItem* fileList = findItem(QStringLiteral("fileList"));
    QVERIFY(fileList);

    pane()->setCurrentIndex(2);
    settle();
    QCOMPARE(fileList->property("currentIndex").toInt(), 2);

    pressKey(Qt::Key_Down);
    QCOMPARE(pane()->currentIndex(), 3);
    QCOMPARE(fileList->property("currentIndex").toInt(), 3);
}

void TestKeyboardNavigation::enterOpensTheRightRowAfterNavigating()
{
    // The reported symptom: after moving around, Enter opened whatever had
    // been selected earlier rather than the row under the cursor.
    pane()->setCurrentIndex(2); // "gamma"
    settle();
    QCOMPARE(pane()->files()->nameAt(2), QStringLiteral("gamma"));

    // Go somewhere and come back, which resets the model underneath the view.
    pressKey(Qt::Key_Return);
    QVERIFY(waitFor(
        [this] {
            return pane()->currentUri() == m_tree->rootUri().child(QStringLiteral("gamma")).toString();
        },
        5000));

    pressKey(Qt::Key_Backspace);
    QVERIFY(waitFor(
        [this] {
            return pane()->currentUri() == m_tree->rootUri().toString() && pane()->files()->rowCount() == 5;
        },
        5000));

    // Stepping up lands back on the folder just left, so walking a tree feels
    // like walking rather than restarting at the top of each level.
    QVERIFY(waitFor([this] { return pane()->currentIndex() == 2; }, 5000));
    QCOMPARE(pane()->files()->nameAt(pane()->currentIndex()), QStringLiteral("gamma"));

    // Aim one row further down and open that instead. This is the actual
    // subject: Enter must open what the cursor is on, not what it used to be on.
    pressKey(Qt::Key_Up);
    QCOMPARE(pane()->currentIndex(), 1);
    QCOMPARE(pane()->files()->nameAt(1), QStringLiteral("beta"));

    pressKey(Qt::Key_Return);
    QVERIFY2(waitFor(
                 [this] {
                     return pane()->currentUri()
                         == m_tree->rootUri().child(QStringLiteral("beta")).toString();
                 },
                 5000),
        "Enter must open the row under the cursor, not an earlier one");
}

void TestKeyboardNavigation::ctrlArrowsNavigateHistory()
{
    const QString root = m_tree->rootUri().toString();
    const QString alpha = m_tree->rootUri().child(QStringLiteral("alpha")).toString();

    pressKey(Qt::Key_Return); // into alpha/
    QVERIFY(waitFor([this, alpha] { return pane()->currentUri() == alpha; }, 5000));

    // Ctrl + arrows do what the three toolbar buttons do.
    pressKey(Qt::Key_Left, Qt::ControlModifier);
    QVERIFY(waitFor([this, root] { return pane()->currentUri() == root; }, 5000));

    pressKey(Qt::Key_Right, Qt::ControlModifier);
    QVERIFY(waitFor([this, alpha] { return pane()->currentUri() == alpha; }, 5000));

    pressKey(Qt::Key_Up, Qt::ControlModifier);
    QVERIFY(waitFor([this, root] { return pane()->currentUri() == root; }, 5000));

    // A bare Up still moves the cursor rather than navigating.
    pane()->setCurrentIndex(2);
    settle();
    pressKey(Qt::Key_Up);
    QCOMPARE(pane()->currentIndex(), 1);
    QCOMPARE(pane()->currentUri(), root);
}

void TestKeyboardNavigation::typingStartsFilteringWithoutAShortcut()
{
    // No shortcut to remember: the first printable keystroke opens the filter
    // and goes into it, so nothing the user typed is lost.
    QCOMPARE(pane()->files()->rowCount(), 5);

    pressKey(Qt::Key_B); // "beta"
    QVERIFY(waitFor([this] { return pane()->files()->rowCount() == 1; }, 3000));
    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("beta"));

    QQuickItem* filter = findItem(QStringLiteral("filterField"));
    QVERIFY(filter);
    QVERIFY2(filter->hasActiveFocus(), "the keyboard must move into the filter");
    QCOMPARE(filter->property("text").toString(), QStringLiteral("b"));

    // Escape puts everything back.
    pressKey(Qt::Key_Escape);
    QVERIFY(waitFor([this] { return pane()->files()->rowCount() == 5; }, 3000));
}

void TestKeyboardNavigation::modifiedKeysDoNotStartFiltering()
{
    // Ctrl+D must add a bookmark, not type a "d" into a filter.
    pressKey(Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(pane()->files()->rowCount(), 5);

    QQuickItem* filter = findItem(QStringLiteral("filterField"));
    QVERIFY(filter);
    QVERIFY(!filter->hasActiveFocus());
}

void TestKeyboardNavigation::f3OpensAPreviewAndReusesTheTab()
{
    // Cursor onto a file rather than a folder.
    const int row = pane()->files()->rowOfUri(m_tree->rootUri().child(QStringLiteral("one.txt")).toString());
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    settle();

    const int before = m_app->tabs()->rowCount();
    pressKey(Qt::Key_F3);
    QCOMPARE(m_app->tabs()->rowCount(), before + 1);

    QObject* preview = m_app->tabs()->currentController();
    QVERIFY(preview);
    QCOMPARE(preview->property("currentUri").toString(),
        m_tree->rootUri().child(QStringLiteral("one.txt")).toString());

    // A second F3 reuses the tab instead of piling one up per file.
    m_app->tabs()->setCurrentIndex(0);
    settle();
    pane()->setCurrentIndex(
        pane()->files()->rowOfUri(m_tree->rootUri().child(QStringLiteral("two.txt")).toString()));
    settle();
    pressKey(Qt::Key_F3);

    QCOMPARE(m_app->tabs()->rowCount(), before + 1);
    QCOMPARE(m_app->tabs()->currentController()->property("currentUri").toString(),
        m_tree->rootUri().child(QStringLiteral("two.txt")).toString());
}

void TestKeyboardNavigation::f3OnAFolderOpensItInstead()
{
    // Held onto, because pane() asks the *current* tab for its pane and the
    // second half of this test opens a preview -- at which point the current tab
    // is not a browser and pane() is null.
    BrowserPaneController* browser = pane();
    QVERIFY(browser);

    // Folders sort first, so row 0 is one. Nothing to preview there, and a key
    // that does nothing cannot be told apart from a key that is broken.
    QCOMPARE(browser->currentIndex(), 0);
    QVERIFY(browser->files()->isDirAt(0));

    const int tabsBefore = m_app->tabs()->rowCount();
    pressKey(Qt::Key_F3);

    QVERIFY2(waitFor([browser] { return browser->currentUri().endsWith(QStringLiteral("/alpha")); }, 5000),
        "F3 on a folder opens it, the same as Return");
    QCOMPARE(m_app->tabs()->rowCount(), tabsBefore);

    // And on a file it is still a preview: a tab opens and the listing stays put.
    QVERIFY(waitFor([browser] { return browser->files()->rowCount() == 1; }, 5000));
    const QString here = browser->currentUri();
    pressKey(Qt::Key_F3);
    QCOMPARE(m_app->tabs()->rowCount(), tabsBefore + 1);
    QCOMPARE(browser->currentUri(), here);
}

void TestKeyboardNavigation::previewArrowsStepThroughTheFolder()
{
    m_app->previewFile(m_tree->rootUri().child(QStringLiteral("one.txt")).toString());
    settle();

    QObject* preview = m_app->tabs()->currentController();
    QVERIFY(preview);

    // The neighbours load in the background; the arrows need them.
    QVERIFY(waitFor([preview] { return preview->property("siblingCount").toInt() >= 2; }, 5000));

    // Files only, in name order: one.txt then two.txt. Folders are skipped
    // because stepping into one from a preview means nothing.
    QCOMPARE(preview->property("fileName").toString(), QStringLiteral("one.txt"));
    QVERIFY(preview->property("canGoNext").toBool());
    QVERIFY(!preview->property("canGoPrevious").toBool());

    QMetaObject::invokeMethod(preview, "next");
    settle();
    QCOMPARE(preview->property("fileName").toString(), QStringLiteral("two.txt"));
    QVERIFY(preview->property("canGoPrevious").toBool());

    QMetaObject::invokeMethod(preview, "previous");
    settle();
    QCOMPARE(preview->property("fileName").toString(), QStringLiteral("one.txt"));

    // Stepping past the end is a no-op, not a crash.
    QMetaObject::invokeMethod(preview, "previous");
    settle();
    QCOMPARE(preview->property("fileName").toString(), QStringLiteral("one.txt"));
}

void TestKeyboardNavigation::newTabShortcutOpensATab()
{
    const int before = m_app->tabs()->rowCount();
    pressKey(Qt::Key_T, Qt::ControlModifier);
    QCOMPARE(m_app->tabs()->rowCount(), before + 1);
}

QObject* TestKeyboardNavigation::findObject(const QString& objectName) const
{
    if (QObject* root = m_engine->rootObjects().value(0))
        return root->findChild<QObject*>(objectName);
    return nullptr;
}

void TestKeyboardNavigation::f4MenuWalksIntoSubmenusWithTheKeyboard()
{
    // `opened` rather than `visible`, and waited for: the enter transition runs
    // after the popup is shown, and a menu mid-animation has not got the
    // keyboard yet.
    const auto isOpen = [](QObject* target) {
        return waitFor([target] { return target->property("opened").toBool(); }, 3000);
    };
    const auto isClosed = [](QObject* target) {
        return waitFor([target] { return !target->property("opened").toBool(); }, 3000);
    };

    pressKey(Qt::Key_F4);
    QObject* menu = findObject(QStringLiteral("appMenu"));
    QVERIFY(menu);
    QVERIFY2(isOpen(menu), "F4 opens the menu");

    // Down highlights the first heading. Without this the menu is open and inert
    // until something is clicked, which is the whole complaint.
    pressKey(Qt::Key_Down);
    QCOMPARE(menu->property("currentIndex").toInt(), 0);

    QObject* fileMenu = findObject(QStringLiteral("menuFile"));
    QVERIFY(fileMenu);
    QVERIFY(!fileMenu->property("opened").toBool());

    pressKey(Qt::Key_Right);
    QVERIFY2(isOpen(fileMenu), "Right has to open the highlighted submenu");

    // Opening it highlights its first entry, so the keyboard is inside it and
    // the next Down moves within the submenu rather than along the headings.
    QCOMPARE(fileMenu->property("currentIndex").toInt(), 0);
    pressKey(Qt::Key_Down);
    QCOMPARE(fileMenu->property("currentIndex").toInt(), 1);

    // Left comes back out to the headings without closing everything, which is
    // what makes walking the menu possible rather than a one-way trip.
    pressKey(Qt::Key_Left);
    QVERIFY2(isClosed(fileMenu), "Left leaves the submenu");
    QVERIFY2(menu->property("opened").toBool(), "and the menu it belongs to stays open");
    QVERIFY2(waitFor([menu] { return menu->property("activeFocus").toBool(); }, 3000),
        "and gets the keyboard back, or the arrows do nothing from here on");

    // A second heading, opened with Enter this time: both keys mean "go in".
    pressKey(Qt::Key_Down);
    QCOMPARE(menu->property("currentIndex").toInt(), 1);
    pressKey(Qt::Key_Return);
    QObject* viewMenu = findObject(QStringLiteral("menuView"));
    QVERIFY(viewMenu);
    QVERIFY2(isOpen(viewMenu), "Enter opens a submenu as well");

    pressKey(Qt::Key_Escape);
    pressKey(Qt::Key_Escape);
    QVERIFY2(isClosed(menu), "Escape gets out of the menu entirely");
}

// The dialog that destroys things has to say what it is about to destroy. Delete
// is the one operation with no second chance, and it used to ask "delete 2 items?"
// -- which is exactly as much as somebody already knew before pressing the key.
void TestKeyboardNavigation::deleteAsksWithTheFilesNamed()
{
    // Two files ticked, and neither of them the one under the cursor when the
    // question is finally asked -- so a dialog reading the cursor rather than the
    // selection would list the wrong thing and be caught here.
    pane()->cursorToEnd();
    pressKey(Qt::Key_Insert);
    pressKey(Qt::Key_Up);
    pressKey(Qt::Key_Insert);
    QCOMPARE(pane()->files()->selectionCount(), 2);

    pressKey(Qt::Key_Delete);
    // Through the visual tree: a Popup is not a child of the window in the object
    // sense, but its contents are in the overlay once it is up.
    QQuickItem* listed = nullptr;
    QVERIFY2(waitFor(
                 [this, &listed] {
                     listed = findItem(QStringLiteral("deleteTargetList"));
                     return listed != nullptr;
                 },
                 3000),
        "the dialog lists what it would delete");
    settle();

    QCOMPARE(listed->property("count").toInt(), 2);

    QStringList named;
    for (const QVariant& row : listed->property("model").toList())
        named.append(row.toMap().value(QStringLiteral("name")).toString());
    named.sort();
    QCOMPARE(named, QStringList({ QStringLiteral("one.txt"), QStringLiteral("two.txt") }));

    // Taken when the question was asked. Whatever happens in the listing behind
    // the dialog, the rows agreed to are the rows that were shown.
    pane()->files()->clearSelection();
    settle();
    QCOMPARE(listed->property("count").toInt(), 2);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestKeyboardNavigation testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_KeyboardNavigation.moc"
