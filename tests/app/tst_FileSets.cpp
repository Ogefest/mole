#include "plugins/builtin/AnalysisFeature.h"
#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/BulkRenameFeature.h"
#include "plugins/builtin/FileSetsFeature.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/DriveListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/sets/FileSetStore.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Sets, and the thing that makes them worth having: an operation can act on one
/// without knowing what a set is.
class TestFileSets : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void createsAndRemembersASet();
    void refusesADuplicateMember();
    void reportsWhichDrivesItSpans();
    void survivesARestart();

    void aSetNamesItsTargetsTheSameWayAPaneDoes();
    void anOperationTakesASetWithoutKnowingWhatOneIs();
    void aFilteredSetNamesTheRowsTheFilterLeft();
    void compressingAFilteredSetPacksWhatWasOnScreen();
    void addToSetTakesWhateverTheCurrentTabIsAimedAt();
    void addToSetTwiceLeavesOneSetsTab();
    void twoBrowsersAreStillTwoBrowsersAndTwoSearchesTwoSearches();

    void bulkRenameTakesASetLikeAnyOtherOperation();
    void bulkRenameRefusesABatchThatWouldCollide();
    void bulkRenameSaysWhichRenamesFailedAndKeepsTheRules();
    void aBatchSpanningTwoDrivesUsesEachDrivesOwnNameRules();
    void addsADriveThroughTheSameFormEveryBackendDeclares();
    void aSavedPasswordIsNeverInTheSettingsFile();
    void savingADriveChecksItStraightAway();
    void copyingThisFoldersPathGivesANativePath();
    void copyingTheSelectedFilesPathUsesTheRowUnderTheCursor();
    void copyingAFilePathDoesNothingWhenAFolderIsUnderTheCursor();
    void copyingTheDrivesPathGivesTheMountRoot();
    void aRemoteLocationIsCopiedAsAUriNotAsAPathThatLooksLocal();
    void verifyFindsMembersThatHaveGone();
    void forgettingMissingLeavesTheRestAlone();

private:
    FileSetsController* openSets();
    /// How many tabs of one kind are open.
    int tabsOfFeature(const QString& featureId) const;

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestFileSets::initTestCase()
{
    QVERIFY(m_profile.isValid());

    // The drives this build offers come from loadable plugins now -- the network
    // backends are no longer compiled into the application -- so the host has to
    // be pointed at them or there would be nothing to configure.
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));
}

void TestFileSets::init()
{
    m_profile.clearVolatileState();
    QFile::remove(m_profile.filePath(QStringLiteral("sets.json")));
    QFile::remove(m_profile.filePath(QStringLiteral("credentials.enc")));
    QFile::remove(m_profile.filePath(QStringLiteral("drives.json")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray(100, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray(200, 'b')));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/c.txt"), QByteArray(300, 'c')));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
}

void TestFileSets::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

FileSetsController* TestFileSets::openSets()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.filesets"));
    return row < 0 ? nullptr : qobject_cast<FileSetsController*>(m_app->tabs()->controllerAt(row));
}

int TestFileSets::tabsOfFeature(const QString& featureId) const
{
    int found = 0;
    for (int row = 0; row < m_app->tabs()->rowCount(); ++row) {
        if (m_app->tabs()->index(row, 0).data(TabsModel::FeatureIdRole).toString() == featureId)
            ++found;
    }
    return found;
}

void TestFileSets::createsAndRemembersASet()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);

    const QString id = sets->createSet(QStringLiteral("Reading list"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(sets->currentSetId(), id);
    QCOMPARE(sets->currentName(), QStringLiteral("Reading list"));
    QCOMPARE(sets->memberCount(), 0);

    QCOMPARE(sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
                 m_tree->rootUri().child(QStringLiteral("b.txt")).toString() }),
        2);
    QCOMPARE(sets->memberCount(), 2);

    // A name is required; an unnamed set is one nobody can find again.
    QVERIFY(sets->createSet(QStringLiteral("   ")).isEmpty());
}

void TestFileSets::refusesADuplicateMember()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Set"));

    const QString a = m_tree->rootUri().child(QStringLiteral("a.txt")).toString();
    QCOMPARE(sets->addUris({ a }), 1);

    // A set is a set. A duplicate would make every operation act on it twice --
    // copying it twice, deleting it twice, counting it twice in a report.
    QCOMPARE(sets->addUris({ a }), 0);
    QCOMPARE(sets->memberCount(), 1);
}

void TestFileSets::reportsWhichDrivesItSpans()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Mixed"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        QStringLiteral("mem://scratch/elsewhere.txt") });

    // Crossing drives is normal for a set and worth saying, because most
    // operations on it then involve a transfer rather than a local move.
    const QVariantList listed = sets->sets();
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.first().toMap().value(QStringLiteral("driveCount")).toInt(), 2);
}

void TestFileSets::survivesARestart()
{
    {
        FileSetsController* sets = openSets();
        QVERIFY(sets);
        sets->createSet(QStringLiteral("Kept"));
        sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString() });
    }

    m_app.reset();
    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    FileSetsController* sets = openSets();
    QVERIFY(sets);
    QCOMPARE(sets->sets().size(), 1);
    QCOMPARE(sets->currentName(), QStringLiteral("Kept"));
    QCOMPARE(sets->memberCount(), 1);
}

// ---- the point of the whole thing --------------------------------------

void TestFileSets::aSetNamesItsTargetsTheSameWayAPaneDoes()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Work"));

    const QString a = m_tree->rootUri().child(QStringLiteral("a.txt")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();
    sets->addUris({ a, docs });

    // The same question, the same answer shape. This is what lets an operation
    // take a set without a second code path -- and what would break the moment
    // someone gave sets their own accessor.
    QCOMPARE(sets->targetUris(), QStringList({ a, docs }));
    QCOMPARE(sets->targetCount(), 2);
    QCOMPARE(sets->targets().size(), 2);
    QCOMPARE(sets->targets().first().toString(), a);
}

void TestFileSets::anOperationTakesASetWithoutKnowingWhatOneIs()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Folders"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("docs")).toString() });

    // The shell asks the current tab what it is aimed at. It never asks whether
    // that tab is a set.
    QCOMPARE(
        m_app->currentTargets(), QStringList { m_tree->rootUri().child(QStringLiteral("docs")).toString() });

    // And the analysis operation, written long before sets existed, acts on it.
    m_app->analyseSelection();
    auto* analysis = qobject_cast<AnalysisTabController*>(m_app->tabs()->currentController());
    QVERIFY2(analysis, "analysing a set opens the ordinary analysis tab");
    QCOMPARE(analysis->targetCount(), 1);
    QVERIFY(waitFor(
        [analysis] {
            return analysis->current() && analysis->current()->hasReport() && !analysis->current()->isBusy();
        },
        30000));
    QCOMPARE(analysis->current()->headline().value(QStringLiteral("files")).toLongLong(), 1);
}

/// The filter is part of choosing, not only a way of looking.
///
/// A set of five hundred, narrowed to three by typing ".log" into the filter
/// box: targetUris() answered all five hundred, so compressing produced an
/// archive of five hundred, a bulk rename renamed five hundred, and "remove the
/// sources afterwards" deleted five hundred. Nothing said so. The search tab has
/// always answered with the rows on screen -- "the rows in front of the user are
/// what 'these results' means" -- and the two tabs answering the same question
/// disagreed. Settled in ARCHITECTURE.md's "Acting on a list of things". See
/// MOLE-408.
void TestFileSets::aFilteredSetNamesTheRowsTheFilterLeft()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Mixed"));

    QStringList all;
    for (const QString& name : { QStringLiteral("one.log"), QStringLiteral("two.log"),
             QStringLiteral("three.txt"), QStringLiteral("four.txt"), QStringLiteral("five.txt") }) {
        QVERIFY(m_tree->writeFile(name));
        all.append(m_tree->rootUri().child(name).toString());
    }
    sets->addUris(all);
    QCOMPARE(sets->targetUris().size(), 5);

    sets->setFilter(QStringLiteral(".log"));

    // The rows on screen, and the targets, are the same two.
    QStringList shown;
    for (const QVariant& row : sets->members())
        shown.append(row.toMap().value(QStringLiteral("uri")).toString());
    QCOMPARE(shown.size(), 2);
    QCOMPARE(sets->targetUris(), shown);
    QCOMPARE(sets->targetCount(), 2);
    QCOMPARE(sets->targets().size(), 2);

    // And the shell, which asks the tab by name, gets the same answer.
    QCOMPARE(m_app->currentTargets(), shown);

    // The set itself still holds everything: filtering is a way of choosing what
    // to act on, not a way of editing what is in the set.
    QCOMPARE(sets->memberCount(), 5);
    sets->setFilter(QString());
    QCOMPARE(sets->targetUris().size(), 5);
}

/// The same rule, through the operation the fault was found with.
void TestFileSets::compressingAFilteredSetPacksWhatWasOnScreen()
{
    if (!m_app->canCompress())
        QSKIP("this build was made without libarchive");

    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Logs and the rest"));

    QStringList all;
    for (const QString& name : { QStringLiteral("keep.log"), QStringLiteral("leave-a.txt"),
             QStringLiteral("leave-b.txt"), QStringLiteral("leave-c.txt") }) {
        QVERIFY(m_tree->writeFile(name, name.toUtf8()));
        all.append(m_tree->rootUri().child(name).toString());
    }
    sets->addUris(all);
    sets->setFilter(QStringLiteral(".log"));

    // What the dialog would say it is about to pack, which is the same list.
    QCOMPARE(m_app->compressionTargets().size(), 1);

    m_app->compressSelection(QStringLiteral("filtered.zip"), QStringLiteral("zip"));

    const QString archive = m_tree->absolute(QStringLiteral("filtered.zip"));
    QVERIFY2(
        waitFor([archive] { return QFileInfo::exists(archive) && QFileInfo(archive).size() > 0; }, 30000),
        "the archive was never written");
    QVERIFY(waitFor([this] { return m_app->tasks()->finishedCount() > 0; }, 30000));

    // Read with unzip rather than through Mole's own archive backend, which is a
    // loadable plugin this suite does not link: what is in question is what was
    // written, and an outside reader is the better witness for that anyway.
    const QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
    if (unzip.isEmpty())
        QSKIP("unzip is not available to read the archive back");

    QProcess reader;
    reader.start(unzip, { QStringLiteral("-Z1"), archive });
    QVERIFY(reader.waitForFinished(30000));
    const QStringList packed
        = QString::fromUtf8(reader.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    // One member, and it is the one that was on screen.
    QCOMPARE(packed, QStringList { QStringLiteral("keep.log") });
}

void TestFileSets::addToSetTakesWhateverTheCurrentTabIsAimedAt()
{
    // Selected in a browser, added from the menu -- the shell reads the pane's
    // selection through the same path a set answers.
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("a.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->files()->toggleSelected(row);

    QCOMPARE(m_app->addToSet({}, QStringLiteral("From the browser")), 1);
    QCOMPARE(m_app->sets()->sets().size(), 1);
    QCOMPARE(m_app->sets()->sets().first().count(), 1);
}

/// Ctrl+Shift+S twice used to leave two Sets tabs, three times three -- each with
/// its own controller over the same store and its own idea of which set is
/// current. ADR-0032 calls the sets a standing tool that exists once, and a
/// second tab of one is a duplicate of the first. See MOLE-206.
void TestFileSets::addToSetTwiceLeavesOneSetsTab()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));
    FileListModel* files = browser->activePane()->files();

    const int a = files->rowOfUri(m_tree->rootUri().child(QStringLiteral("a.txt")).toString());
    const int b = files->rowOfUri(m_tree->rootUri().child(QStringLiteral("b.txt")).toString());
    QVERIFY(a >= 0);
    QVERIFY(b >= 0);

    files->toggleSelected(a);
    QVERIFY(m_app->triggerAction(QStringLiteral("mole.tools.addToSet")));
    QCOMPARE(tabsOfFeature(QStringLiteral("core.filesets")), 1);

    // Back to the browser for the second lot: from the Sets tab the shell is
    // aimed at the set's own members, which is a different question.
    m_app->tabs()->setCurrentIndex(0);
    files->clearSelection();
    files->toggleSelected(b);
    QVERIFY(m_app->triggerAction(QStringLiteral("mole.tools.addToSet")));

    QCOMPARE(tabsOfFeature(QStringLiteral("core.filesets")), 1);
    QCOMPARE(m_app->sets()->sets().size(), 1);
    QCOMPARE(m_app->sets()->sets().first().count(), 2);
}

/// The reuse is for the tabs that are asked for by name as standing tools, and
/// for nothing else. A browser and a search are things people open several of.
void TestFileSets::twoBrowsersAreStillTwoBrowsersAndTwoSearchesTwoSearches()
{
    const int browsers = tabsOfFeature(QStringLiteral("mole.browser"));
    QVERIFY(m_app->tabs()->openTab(QStringLiteral("mole.browser")) >= 0);
    QVERIFY(m_app->tabs()->openTab(QStringLiteral("mole.browser")) >= 0);
    QCOMPARE(tabsOfFeature(QStringLiteral("mole.browser")), browsers + 2);

    QVERIFY(m_app->tabs()->openTab(QStringLiteral("mole.livesearch")) >= 0);
    QVERIFY(m_app->tabs()->openTab(QStringLiteral("mole.livesearch")) >= 0);
    QCOMPARE(tabsOfFeature(QStringLiteral("mole.livesearch")), 2);
}

// ---- a set outlives the files in it ------------------------------------

void TestFileSets::verifyFindsMembersThatHaveGone()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Watched"));

    const QString a = m_tree->rootUri().child(QStringLiteral("a.txt")).toString();
    const QString b = m_tree->rootUri().child(QStringLiteral("b.txt")).toString();
    sets->addUris({ a, b });

    QVERIFY(QFile::remove(QDir(m_tree->path()).filePath(QStringLiteral("b.txt"))));

    sets->verify();
    QVERIFY(waitFor([sets] { return sets->missingCount() == 1; }, 10000));

    // "Not checked yet" and "not there" are different states: reporting a
    // healthy set as broken before anything had looked would be worse than
    // saying nothing.
    const QVariantList members = sets->members();
    QCOMPARE(members.size(), 2);
    QCOMPARE(members.at(0).toMap().value(QStringLiteral("missing")).toBool(), false);
    QCOMPARE(members.at(0).toMap().value(QStringLiteral("checked")).toBool(), true);
    QCOMPARE(members.at(1).toMap().value(QStringLiteral("missing")).toBool(), true);
}

void TestFileSets::forgettingMissingLeavesTheRestAlone()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Watched"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        m_tree->rootUri().child(QStringLiteral("b.txt")).toString() });

    QVERIFY(QFile::remove(QDir(m_tree->path()).filePath(QStringLiteral("b.txt"))));
    sets->verify();
    QVERIFY(waitFor([sets] { return sets->missingCount() == 1; }, 10000));

    QCOMPARE(sets->forgetMissing(), 1);
    QCOMPARE(sets->memberCount(), 1);
    QCOMPARE(sets->targetUris(), QStringList { m_tree->rootUri().child(QStringLiteral("a.txt")).toString() });
}

void TestFileSets::bulkRenameTakesASetLikeAnyOtherOperation()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Rename me"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        m_tree->rootUri().child(QStringLiteral("b.txt")).toString() });

    // Opened from the set tab, so the shell hands it the set's members through
    // exactly the path a pane's selection uses.
    const int row = m_app->openFeatureTab(QStringLiteral("core.bulkrename"));
    QVERIFY(row >= 0);
    auto* rename = qobject_cast<BulkRenameController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(rename);
    QCOMPARE(rename->sourceCount(), 2);

    rename->addRule(QStringLiteral("affix"));
    rename->setRuleField(0, QStringLiteral("prefix"), QStringLiteral("2024_"));

    // The preview is the feature: it says what would happen before it does.
    QCOMPARE(rename->changedCount(), 2);
    QCOMPARE(rename->blockedCount(), 0);
    QVERIFY(rename->canApply());
    QCOMPARE(rename->preview().first().toMap().value(QStringLiteral("to")).toString(),
        QStringLiteral("2024_a.txt"));

    rename->apply();
    QVERIFY(waitFor(
        [this] {
            return QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("2024_a.txt")))
                && QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("2024_b.txt")));
        },
        10000));
    QVERIFY(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("a.txt"))));

    // And it re-aims at what now exists, rather than leaving a preview that
    // refers to files that are gone.
    QVERIFY(waitFor(
        [rename] {
            return rename->sourceUris().contains(QStringLiteral("2024_a.txt"), Qt::CaseInsensitive)
                || rename->sourceUris().join(QChar()).contains(QStringLiteral("2024_a.txt"));
        },
        5000));
}

void TestFileSets::bulkRenameRefusesABatchThatWouldCollide()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Colliding"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        m_tree->rootUri().child(QStringLiteral("b.txt")).toString() });

    const int row = m_app->openFeatureTab(QStringLiteral("core.bulkrename"));
    auto* rename = qobject_cast<BulkRenameController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(rename);

    // Both names become the same thing.
    rename->addRule(QStringLiteral("replace"));
    rename->setRuleField(0, QStringLiteral("find"), QStringLiteral("a"));
    rename->setRuleField(0, QStringLiteral("replaceWith"), QStringLiteral("same"));
    rename->addRule(QStringLiteral("replace"));
    rename->setRuleField(1, QStringLiteral("find"), QStringLiteral("b"));
    rename->setRuleField(1, QStringLiteral("replaceWith"), QStringLiteral("same"));

    QCOMPARE(rename->blockedCount(), 1);
    QVERIFY2(!rename->canApply(), "nothing is renamed until every row can be");

    // And applying anyway does nothing at all -- the original files are intact.
    rename->apply();
    drainEvents();
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("a.txt"))));
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("b.txt"))));
}

/// A batch that half-landed used to look exactly like one that landed whole.
///
/// apply() rebuilt the source list as though every doable rename had succeeded,
/// cleared the rules and said nothing -- while RenameTask::failures(),
/// renamedCount() and state() went unread. So the tab listed names that were
/// never created and had lost the files that still carried their old ones, and
/// the rules that produced the batch were gone. The file removed here between
/// the preview and the apply is the ordinary way this happens: something else
/// touched the folder. See MOLE-377.
void TestFileSets::bulkRenameSaysWhichRenamesFailedAndKeepsTheRules()
{
    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Two files"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        m_tree->rootUri().child(QStringLiteral("b.txt")).toString() });

    const int row = m_app->openFeatureTab(QStringLiteral("core.bulkrename"));
    auto* rename = qobject_cast<BulkRenameController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(rename);
    QVERIFY(waitFor([rename] { return rename->sourceCount() == 2; }));

    rename->addRule(QStringLiteral("affix"));
    rename->setRuleField(0, QStringLiteral("prefix"), QStringLiteral("new-"));
    QCOMPARE(rename->changedCount(), 2);
    QCOMPARE(rename->blockedCount(), 0);
    QVERIFY(rename->canApply());
    QVERIFY(rename->errorText().isEmpty());

    // Gone between the preview and the apply, which is what the preview cannot
    // promise about.
    QVERIFY(QFile::remove(QDir(m_tree->path()).filePath(QStringLiteral("a.txt"))));

    rename->apply();
    QVERIFY(waitFor([rename] { return !rename->isBusy(); }, 10000));
    drainEvents();

    // Said, rather than passed over.
    QVERIFY2(!rename->errorText().isEmpty(), "a rename that failed said nothing");
    QVERIFY2(rename->errorText().contains(QStringLiteral("a.txt")), qPrintable(rename->errorText()));

    // The rules that produced this batch are still there, because somebody has
    // to be able to fix the cause and press the button again.
    QCOMPARE(rename->rules().size(), 1);

    // And the list is what is really on the disk: the one that moved under its
    // new name, the one that did not under its old.
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("new-b.txt"))));
    QVERIFY(waitFor([rename] { return rename->sourceCount() == 2; }));
    const QStringList aimedAt = rename->sourceUris();
    QVERIFY2(aimedAt.contains(m_tree->rootUri().child(QStringLiteral("new-b.txt")).toString()),
        qPrintable(aimedAt.join(QStringLiteral(", "))));
    QVERIFY2(!aimedAt.contains(m_tree->rootUri().child(QStringLiteral("new-a.txt")).toString()),
        "the tab listed a name that was never created");
    QVERIFY2(aimedAt.contains(m_tree->rootUri().child(QStringLiteral("a.txt")).toString()),
        "the tab lost the file that still carries its old name");
}

/// One rule set for two drives.
///
/// refreshDirectoryContents() assigned `m_nameRules = fs->nameRules()` per
/// directory rather than keeping them, so a batch spanning a local disk and a
/// share was previewed with whichever drive the hash-ordered last directory
/// happened to sit on -- inventing refusals for one half or missing them for the
/// other, depending on the order. See MOLE-377.
void TestFileSets::aBatchSpanningTwoDrivesUsesEachDrivesOwnNameRules()
{
    // A drive that refuses what Windows refuses, beside the local temp tree,
    // which refuses almost nothing.
    auto strict = std::make_shared<MemoryFileSystem>();
    strict->setNameRules(NameRules::forPlatform(HostPlatform::Windows));
    strict->addFile(QStringLiteral("/share/report.txt"), QByteArray("x"));
    Mount mount;
    mount.id = QStringLiteral("strict");
    mount.displayName = QStringLiteral("Strict");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = strict;
    QVERIFY(!m_app->services().vfs->addMount(mount).isEmpty());

    FileSetsController* sets = openSets();
    QVERIFY(sets);
    sets->createSet(QStringLiteral("Across two drives"));
    sets->addUris({ m_tree->rootUri().child(QStringLiteral("a.txt")).toString(),
        QStringLiteral("mem:///share/report.txt") });

    const int row = m_app->openFeatureTab(QStringLiteral("core.bulkrename"));
    auto* rename = qobject_cast<BulkRenameController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(rename);
    QVERIFY(waitFor([rename] { return rename->sourceCount() == 2; }));

    // A colon, which Windows refuses and Posix does not.
    rename->addRule(QStringLiteral("affix"));
    rename->setRuleField(0, QStringLiteral("prefix"), QStringLiteral("v1:"));
    QVERIFY(waitFor([rename] { return rename->changedCount() == 2; }));

    // Exactly one row is blocked, and it is the one on the drive that refuses
    // the name. Either half of the old behaviour fails this: whichever rule set
    // won, both rows were judged by it.
    QCOMPARE(rename->blockedCount(), 1);
    for (const QVariant& value : rename->preview()) {
        const QVariantMap entry = value.toMap();
        const bool onTheStrictDrive
            = entry.value(QStringLiteral("uri")).toString().startsWith(QStringLiteral("mem:"));
        QCOMPARE(entry.value(QStringLiteral("blocked")).toBool(), onTheStrictDrive);
    }
}

void TestFileSets::addsADriveThroughTheSameFormEveryBackendDeclares()
{
    // The kinds on offer, and the form for one of them, both come from the
    // backend rather than from anything written by hand here.
    const QVariantList kinds = m_app->driveKinds();
    QVERIFY2(!kinds.isEmpty(), "at least one backend must offer configurable drives");

    QString factory;
    QString variant;
    for (const QVariant& value : kinds) {
        const QVariantMap kind = value.toMap();
        // Matched on either, because how a backend names itself is its own
        // business: SFTP is a factory of its own with no variants, where a
        // factory wrapping several providers would offer it as a variant.
        if (kind.value(QStringLiteral("factory")).toString() == QLatin1String("sftp")
            || kind.value(QStringLiteral("variant")).toString() == QLatin1String("sftp")) {
            factory = kind.value(QStringLiteral("factory")).toString();
            variant = kind.value(QStringLiteral("variant")).toString();
            break;
        }
    }
    if (factory.isEmpty())
        QSKIP("no sftp backend in this build; the network plugin was not built");

    const QVariantList fields = m_app->driveFields(factory, variant);
    QVERIFY(!fields.isEmpty());

    bool sawSecret = false;
    for (const QVariant& value : fields) {
        if (value.toMap().value(QStringLiteral("secret")).toBool())
            sawSecret = true;
    }
    QVERIFY2(sawSecret, "sftp has a password, and the form has to know which field it is");

    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("nas.example.org"));
    values.insert(QStringLiteral("user"), QStringLiteral("ada"));

    QVERIFY(
        m_app->saveDrive({}, QStringLiteral("Test NAS"), factory, variant, QStringLiteral("/data"), values));

    QAbstractItemModel* configured = m_app->configuredDrives();
    QCOMPARE(configured->rowCount(), 1);
    const QString id = configured->data(configured->index(0, 0), DriveListModel::ConfiguredIdRole).toString();
    QCOMPARE(configured->data(configured->index(0, 0), DriveListModel::DisplayNameRole).toString(),
        QStringLiteral("Test NAS"));
    // The uri scheme comes from the name, so it reads as something a person
    // recognises rather than as an opaque id.
    QCOMPARE(m_app->driveConfiguration(id).value(QStringLiteral("uri")).toString(),
        QStringLiteral("testnas://Test NAS/"));
}

void TestFileSets::copyingThisFoldersPathGivesANativePath()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    const QString copied = m_app->copyCurrentFolderPath();

    // A native path, not a file:// uri: this is meant to be pasted into a
    // terminal or a file dialog.
    QCOMPARE(copied, m_tree->path());
    QVERIFY2(!copied.startsWith(QStringLiteral("file:")), qPrintable(copied));
    QCOMPARE(QGuiApplication::clipboard()->text(), copied);
}

void TestFileSets::copyingTheSelectedFilesPathUsesTheRowUnderTheCursor()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("a.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    const QString copied = m_app->copySelectedFilePath();

    QCOMPARE(copied, QDir(m_tree->path()).filePath(QStringLiteral("a.txt")));
    QCOMPARE(QGuiApplication::clipboard()->text(), copied);
}

void TestFileSets::copyingAFilePathDoesNothingWhenAFolderIsUnderTheCursor()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("docs")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    QGuiApplication::clipboard()->setText(QStringLiteral("left alone"));

    // Refused rather than quietly copying the folder instead. Copying something
    // other than what was asked for is worse than copying nothing, because the
    // difference only shows up after it has been pasted somewhere.
    QVERIFY(m_app->copySelectedFilePath().isEmpty());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("left alone"));
}

void TestFileSets::copyingTheDrivesPathGivesTheMountRoot()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    // Into a subfolder, so the drive root and the open folder are not the same
    // thing and the test can tell which one was copied.
    const QString subfolder = m_tree->rootUri().child(QStringLiteral("docs")).toString();
    browser->activePane()->navigateTo(subfolder);
    QVERIFY(waitFor([browser, &subfolder] { return browser->activePane()->currentUri() == subfolder; }));

    const QString folder = m_app->copyCurrentFolderPath();
    const QString drive = m_app->copyDriveRootPath();

    QVERIFY2(!drive.isEmpty(), "the pane is on a mounted drive");
    QVERIFY2(folder != drive, qPrintable(QStringLiteral("folder %1, drive %2").arg(folder, drive)));
    QVERIFY2(folder.startsWith(drive), qPrintable(QStringLiteral("%1 is not under %2").arg(folder, drive)));
    QCOMPARE(QGuiApplication::clipboard()->text(), drive);
}

void TestFileSets::aRemoteLocationIsCopiedAsAUriNotAsAPathThatLooksLocal()
{
    // The path part on its own would read as local and would not be: pasting
    // "/reports/2026" into a terminal means a directory that does not exist,
    // rather than a folder on a drive. Only a uri says where it actually is.
    const QString remote = QStringLiteral("s3://my-bucket/reports/2026");
    QCOMPARE(m_app->pathTextFor(remote), remote);

    const QString local = m_app->pathTextFor(m_tree->rootUri().toString());
    QCOMPARE(local, m_tree->path());

    QVERIFY(m_app->pathTextFor(QString()).isEmpty());
    QVERIFY(m_app->pathTextFor(QStringLiteral("not a uri at all")).isEmpty());
}

void TestFileSets::savingADriveChecksItStraightAway()
{
    // The point of checking at save time: nothing about saving a drive used to
    // touch the far end, and neither did connecting it, so a wrong endpoint or a
    // refused password only surfaced once something tried to read -- several
    // steps away from the form that caused it.
    const QVariantList kinds = m_app->driveKinds();
    QString factory;
    QString variant;
    for (const QVariant& value : kinds) {
        const QVariantMap kind = value.toMap();
        if (kind.value(QStringLiteral("factory")).toString() == QLatin1String("sftp")
            || kind.value(QStringLiteral("variant")).toString() == QLatin1String("sftp")) {
            factory = kind.value(QStringLiteral("factory")).toString();
            variant = kind.value(QStringLiteral("variant")).toString();
            break;
        }
    }
    if (factory.isEmpty())
        QSKIP("no sftp backend in this build; the network plugin was not built");

    QSignalSpy checked(m_app.get(), &AppController::driveChecked);

    QVariantMap values;
    // A host that cannot exist: the check has to report a verdict either way, and
    // a reserved name gives a fast, offline answer.
    values.insert(QStringLiteral("host"), QStringLiteral("nothing.invalid"));
    values.insert(QStringLiteral("user"), QStringLiteral("someone"));
    values.insert(QStringLiteral("password"), QStringLiteral("whatever"));
    // Asynchronous since MOLE-343: the derivation runs on a task so the window
    // stays live, and the answer arrives as a signal. Waited for on the signal
    // and not on credentialsUnlocked(), which the store sets on the worker
    // thread -- everything the controller then does about it, connecting the
    // drives that were waiting included, happens after that.
    QSignalSpy opened(m_app.get(), &AppController::credentialsAttempted);
    m_app->unlockCredentials(QStringLiteral("a passphrase"));
    QVERIFY(opened.wait(30000));
    QVERIFY(m_app->credentialsUnlocked());
    QVERIFY(m_app->saveDrive({}, QStringLiteral("Nowhere"), factory, variant, {}, values));

    // Saving succeeds regardless -- a failed check must not throw away what was
    // typed -- but the verdict has to arrive on its own, without anyone asking.
    QVERIFY2(checked.wait(60000), "saving a drive has to check it");
    QCOMPARE(checked.count(), 1);
    QVERIFY2(!checked.first().at(1).toBool(), "a host that does not exist is not reachable");
    QVERIFY2(!checked.first().at(2).toString().isEmpty(),
        "and the reason has to be something a reader can act on");
}

void TestFileSets::aSavedPasswordIsNeverInTheSettingsFile()
{
    if (!m_app->credentialsAvailable())
        QSKIP("this build cannot encrypt");

    const QVariantList kinds = m_app->driveKinds();
    QString factory;
    QString variant;
    for (const QVariant& value : kinds) {
        const QVariantMap kind = value.toMap();
        // Matched on either, because how a backend names itself is its own
        // business: SFTP is a factory of its own with no variants, where a
        // factory wrapping several providers would offer it as a variant.
        if (kind.value(QStringLiteral("factory")).toString() == QLatin1String("sftp")
            || kind.value(QStringLiteral("variant")).toString() == QLatin1String("sftp")) {
            factory = kind.value(QStringLiteral("factory")).toString();
            variant = kind.value(QStringLiteral("variant")).toString();
            break;
        }
    }
    if (factory.isEmpty())
        QSKIP("no sftp backend in this build; the network plugin was not built");

    // Asynchronous since MOLE-343: the derivation runs on a task so the window
    // stays live, and the answer arrives as a signal. Waited for on the signal
    // and not on credentialsUnlocked(), which the store sets on the worker
    // thread -- everything the controller then does about it, connecting the
    // drives that were waiting included, happens after that.
    QSignalSpy opened(m_app.get(), &AppController::credentialsAttempted);
    m_app->unlockCredentials(QStringLiteral("a passphrase"));
    QVERIFY(opened.wait(30000));
    QVERIFY(m_app->credentialsUnlocked());

    // The field names are the backend's own: "password" is what the SFTP form
    // declares, and using anything else would be testing a form nobody offers.
    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("nas.example.org"));
    values.insert(QStringLiteral("password"), QStringLiteral("hunter2-not-in-the-file"));
    // A key no backend declared. It must not reach the settings file either: a
    // caller that could add keys of its own choosing could put a secret in a file
    // meant to be readable, and no field would have marked it secret.
    values.insert(QStringLiteral("smuggled"), QStringLiteral("hunter2-not-in-the-file"));
    QVERIFY(m_app->saveDrive({}, QStringLiteral("Secret NAS"), factory, variant, {}, values));

    // The settings file is meant to be read, diffed and backed up. The password
    // is not in it -- only the fact that the field has one.
    QFile settings(m_profile.filePath(QStringLiteral("drives.json")));
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QByteArray plain = settings.readAll();
    QVERIFY2(!plain.contains("hunter2-not-in-the-file"), "the password must not be here");
    QVERIFY2(!plain.contains("smuggled"), "and neither must a key no backend declared");
    QVERIFY(plain.contains("nas.example.org"));
    QVERIFY(plain.contains("password"));

    // Nor in the encrypted one, in readable form.
    QFile encrypted(m_profile.filePath(QStringLiteral("credentials.enc")));
    QVERIFY(encrypted.open(QIODevice::ReadOnly));
    QVERIFY2(!encrypted.readAll().contains("hunter2-not-in-the-file"),
        "and it must be encrypted where it does live");
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestFileSets testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_FileSets.moc"
