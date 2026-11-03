#include "plugins/builtin/BrowserFeature.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include <QQuickItem>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Show hidden files, with a window behind it: the Hidden checkbox and the View
/// menu's entry are two ways to one setting, and each shows what the other set.
///
/// They were two settings, or rather none: the checkbox set both panes of the
/// current tab, the menu flipped the active pane alone, nothing remembered either,
/// and a new tab started with hidden files hidden again. tst_AppIntegration holds
/// the setting across tabs and a restart without a window; this is the half that
/// needs one, because the checkbox is QML. See MOLE-441.
class TestHiddenFiles : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theCheckboxAndTheMenuAreOneSetting();

private:
    BrowserController* browserAt(int tab) const;
    /// The Hidden checkbox of the tab on screen. Every browser tab has one, so
    /// the visible one is the one that counts.
    QQuickItem* shownToggle() const;

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestHiddenFiles::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QVERIFY(m_harness->writeFile(QStringLiteral(".hidden-note"), "x"));
    QVERIFY(m_harness->writeFile(QStringLiteral("visible.txt"), "x"));
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
}

void TestHiddenFiles::cleanup()
{
    m_harness.reset();
}

BrowserController* TestHiddenFiles::browserAt(int tab) const
{
    return qobject_cast<BrowserController*>(m_harness->app()->tabs()->controllerAt(tab));
}

QQuickItem* TestHiddenFiles::shownToggle() const
{
    for (QQuickItem* candidate : m_harness->items(QStringLiteral("hiddenToggle"))) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

void TestHiddenFiles::theCheckboxAndTheMenuAreOneSetting()
{
    BrowserController* browser = browserAt(m_harness->app()->tabs()->currentIndex());
    QVERIFY(browser);
    QQuickItem* toggle = shownToggle();
    QVERIFY(toggle);
    QVERIFY(!toggle->property("checked").toBool());

    const auto menuSays = [this] {
        for (const QVariant& section : m_harness->app()->buildMenu()) {
            for (const QVariant& action : section.toMap().value(QStringLiteral("actions")).toList()) {
                const QVariantMap fields = action.toMap();
                if (fields.value(QStringLiteral("id")).toString() == QLatin1String("mole.view.hiddenFiles"))
                    return fields.value(QStringLiteral("checked")).toBool();
            }
        }
        return false;
    };

    // The checkbox turns it on, and the menu shows it on.
    QVERIFY(m_harness->clickOn(toggle));
    QVERIFY(m_harness->until([browser] { return browser->left()->files()->showHidden(); }));
    QVERIFY2(menuSays(), "the menu does not show what the checkbox set");

    // The menu turns it off, and the checkbox shows it off.
    QVERIFY(m_harness->app()->triggerAction(QStringLiteral("mole.view.hiddenFiles")));
    QVERIFY(m_harness->until([toggle] { return !toggle->property("checked").toBool(); }));
    QVERIFY(!browser->left()->files()->showHidden());

    // And back on from the menu, which the checkbox follows too.
    QVERIFY(m_harness->app()->triggerAction(QStringLiteral("mole.view.hiddenFiles")));
    QVERIFY(m_harness->until([toggle] { return toggle->property("checked").toBool(); }));

    // A tab opened now has its own checkbox, and it is already ticked: the
    // setting is not the first tab's.
    QVERIFY(m_harness->app()->triggerAction(QStringLiteral("mole.file.newTab.mole.browser")));
    BrowserController* second = browserAt(m_harness->app()->tabs()->currentIndex());
    QVERIFY(second && second != browser);
    QVERIFY(m_harness->until([this, toggle] { return shownToggle() && shownToggle() != toggle; }));
    QQuickItem* secondToggle = shownToggle();
    QVERIFY2(secondToggle->property("checked").toBool(), "a new tab's checkbox started unticked");
    QVERIFY(second->left()->files()->showHidden());

    // Its checkbox off: both ways in work both ways, and the first tab follows.
    QVERIFY(m_harness->clickOn(secondToggle));
    QVERIFY(m_harness->until([second] { return !second->left()->files()->showHidden(); }));
    QVERIFY(!menuSays());
    QVERIFY2(!browser->left()->files()->showHidden(), "the first tab kept hidden files shown");
}

MOLE_TEST_MAIN_GUI(TestHiddenFiles)
#include "tst_HiddenFiles.moc"
