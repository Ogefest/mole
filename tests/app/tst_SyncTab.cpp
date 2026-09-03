#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/SyncFeature.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The sync tab as the user meets it: compare, read what it says, then agree.
///
/// Mirror is the one operation in this application that deletes things nobody
/// asked it to touch, and the whole guard against that is a list of deletions
/// shown before anything runs. So the claim this file exists to hold is a claim
/// about *those two moments being the same plan*: whatever the confirmation
/// listed is what happens, and nothing else does, however much the trees moved
/// on in between.
class TestSyncTab : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void applyCarriesOutThePlanThatWasShownAndNothingElse();
    void applyingWithNothingComparedRefusesAndSaysSo();
    void aPlanThatHasBeenCarriedOutIsNotOfferedAgain();
    void aTypedPatternReachesTheSessionWithoutAWait();

private:
    SyncController* openSyncTab();
    /// Runs the tab's task to a stop. The controller clears `running` when the
    /// task finishes, which is the condition rather than a wait on a clock.
    bool settle(SyncController* sync);
    bool exists(const QString& relative) const;

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestSyncTab::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestSyncTab::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("src/keep.txt"), QByteArray("kept")));
    QVERIFY(m_tree->writeFile(QStringLiteral("src/doomed.txt"), QByteArray("doomed")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/keep.txt"), QByteArray("kept")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/doomed.txt"), QByteArray("doomed")));
    QVERIFY(m_tree->writeFile(QStringLiteral("dest/stale.txt"), QByteArray("not in the source")));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
}

void TestSyncTab::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

SyncController* TestSyncTab::openSyncTab()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.sync"));
    if (row < 0)
        return nullptr;
    auto* sync = qobject_cast<SyncController*>(m_app->tabs()->controllerAt(row));
    if (!sync)
        return nullptr;
    sync->setSourceUri(m_tree->rootUri().child(QStringLiteral("src")).toString());
    sync->setTargetUri(m_tree->rootUri().child(QStringLiteral("dest")).toString());
    sync->setMode(QStringLiteral("mirror"));
    return sync;
}

bool TestSyncTab::settle(SyncController* sync)
{
    return waitFor([sync] { return !sync->isRunning(); }, 30000);
}

bool TestSyncTab::exists(const QString& relative) const
{
    return QFile::exists(m_tree->absolute(relative));
}

/// The fault this file was written for.
///
/// Compare, and the tab lists one deletion: `stale.txt`. That list is what the
/// confirmation shows and what the user agrees to. Then the source loses a file
/// -- somebody tidying up in another window, a job finishing, a sync from
/// somewhere else -- and Apply runs. Every run used to walk both trees again, so
/// the second walk saw a source without `doomed.txt` and removed the
/// destination's copy: a deletion nobody was ever offered, in the one operation
/// here that destroys things.
void TestSyncTab::applyCarriesOutThePlanThatWasShownAndNothingElse()
{
    SyncController* sync = openSyncTab();
    QVERIFY(sync);

    sync->preview();
    QVERIFY(settle(sync));
    QVERIFY(sync->hasPlan());
    QCOMPARE(sync->deleteCount(), 1);
    QCOMPARE(sync->deletions().size(), 1);

    // What the user is about to agree to, in their own words.
    const QVariantMap shown = sync->deletions().first().toMap();
    QCOMPARE(shown.value(QStringLiteral("name")).toString(), QStringLiteral("stale.txt"));

    QVERIFY(QFile::remove(m_tree->absolute(QStringLiteral("src/doomed.txt"))));

    sync->apply();
    QVERIFY(settle(sync));

    QVERIFY2(!exists(QStringLiteral("dest/stale.txt")), "the deletion that was agreed to had to happen");
    QVERIFY2(exists(QStringLiteral("dest/doomed.txt")),
        "the mirror deleted a file that was not in the plan the confirmation showed");
    QVERIFY(exists(QStringLiteral("dest/keep.txt")));
}

void TestSyncTab::applyingWithNothingComparedRefusesAndSaysSo()
{
    SyncController* sync = openSyncTab();
    QVERIFY(sync);
    QVERIFY(!sync->hasPlan());

    sync->apply();

    QVERIFY2(!sync->isRunning(), "a sync with no plan must not start");
    QVERIFY2(sync->errorText().contains(QStringLiteral("no plan")), qPrintable(sync->errorText()));
    QVERIFY2(exists(QStringLiteral("dest/stale.txt")), "nothing may be deleted without a plan");
}

void TestSyncTab::aPlanThatHasBeenCarriedOutIsNotOfferedAgain()
{
    SyncController* sync = openSyncTab();
    QVERIFY(sync);

    sync->preview();
    QVERIFY(settle(sync));
    QVERIFY(sync->hasPlan());

    sync->apply();
    QVERIFY(settle(sync));

    // The trees are not what the plan described any more -- this run is what
    // changed them -- so agreeing again has to start from a fresh comparison.
    QVERIFY2(!sync->hasPlan(), "a plan that has been carried out is history, not an offer");
}

/// A typed pattern is state, and state that only reaches the file on a clean
/// exit is state that a crash loses.
void TestSyncTab::aTypedPatternReachesTheSessionWithoutAWait()
{
    SyncController* sync = openSyncTab();
    QVERIFY(sync);

    QSignalSpy saved(sync, &FeatureController::stateChanged);
    sync->setExcludePatterns(QStringLiteral("*.tmp"));
    sync->setIncludePatterns(QStringLiteral("*.txt"));
    sync->setSkipNewer(false);
    sync->setRecursive(false);
    sync->setIncludeHidden(true);

    QCOMPARE(saved.count(), 5);
}

MOLE_TEST_MAIN(TestSyncTab)
#include "tst_SyncTab.moc"
