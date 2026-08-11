#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/index/IndexSearchTask.h"
#include "core/index/ScanTask.h"
#include "core/search/LiveSearchTask.h"
#include "core/search/SearchQuery.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>

using namespace mole;
using namespace mole::test;

/// One query, one meaning, one place to prove it.
///
/// A search used to be described twice — `IndexSearchQuery` for the catalogue
/// and `LiveSearchTask::Criteria` for the walk — with the same list of fields
/// in both and nothing keeping them in step. Every criterion this project grows
/// would have been added to both, in two languages, with a third place deciding
/// which of them could answer it. That is where the wrong answers come from.
class TestSearchQuery : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- the evaluator, which is a pure function -------------------------
    void aNameIsMatchedAsASubstring();
    void aNameFoldsBeyondAscii();
    void anExtensionIsExactAndCaseless();
    void aSizeRangeIsInclusive();
    void aDateRangeNeedsADate();
    void aKindIsOneOrTheOther();
    void aPathIsAPrefixOfTheWholeUri();
    void aBlankCriterionIsNotACriterion();
    void aNameCanBeAGlobOrAnExpression();
    void aPatternThatDoesNotCompileMatchesNothing();
    void wholeWordsStopReportMatchingReporting();
    void notOnANameAndOnAPathTakeTheOtherHalf();
    void anExtensionIsOneOfAList();
    void aPathIsSearchedForWhereverItAppears();
    void hiddenIsACriterionOfItsOwn();
    void aTypeClassComesFromWhatIsInTheFile();
    void aTypeClassWithNothingToReadMatchesNothing();
    void datesPeopleTypeBecomeMoments();
    void aDateNobodyCanParseNarrowsNothingRatherThanEverything();
    void createdAndAccessedAreAbsentRatherThanTheEpoch();
    void excludedFoldersAreMatchedAsGlobs();

    // ---- the planner -----------------------------------------------------
    void theIndexTakesWhatSqlCanState();
    void aWalkPushesNothingDownBecauseItHasNowhereToPushIt();
    void whatCannotBePushedDownIsEvaluatedRatherThanDropped();
    void theExpensiveCriterionIsEvaluatedLast();
    void criteriaOfOneCostKeepTheOrderTheyWereWrittenIn();

    // ---- the two engines -------------------------------------------------
    void everyCriterionAnswersTheSameThroughBothEngines();
    void aCriterionTheIndexCannotStateStillNarrowsItsAnswer();

private:
    FileEntry entryFor(const QString& uri, qint64 size = 0, bool isDir = false,
        const QDateTime& modified = QDateTime::fromSecsSinceEpoch(1700000000)) const;

    /// The fixture both engines are asked about: one tree, in memory, scanned
    /// into an index. Anything the two disagree about is a fault in one of them
    /// rather than a difference between two trees.
    bool buildFixture();
    /// Every uri a walk of the fixture returns for `query`, sorted.
    QStringList throughTheWalk(const SearchQuery& query);
    /// The same, from the index.
    QStringList throughTheIndex(const SearchQuery& query);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
    qint64 m_volumeId = -1;
};

void TestSearchQuery::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite")));
    QVERIFY(m_index->open().ok());

    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
}

void TestSearchQuery::cleanup()
{
    m_tasks.reset();
    m_index.reset();
    m_fs.reset();
    m_dir.reset();
    m_volumeId = -1;
}

FileEntry TestSearchQuery::entryFor(
    const QString& uri, qint64 size, bool isDir, const QDateTime& modified) const
{
    FileEntry entry;
    entry.uri = VfsUri::fromString(uri);
    entry.name = entry.uri.fileName();
    entry.size = size;
    entry.isDir = isDir;
    entry.modified = modified;
    return entry;
}

// ---------------------------------------------------------------- the evaluator

void TestSearchQuery::aNameIsMatchedAsASubstring()
{
    const FileEntry entry = entryFor(QStringLiteral("mem:///a/annual-Report.pdf"));

    QVERIFY(SearchPredicate::name(QStringLiteral("report")).matches(entry));
    QVERIFY(SearchPredicate::name(QStringLiteral("Report"), true).matches(entry));
    QVERIFY(!SearchPredicate::name(QStringLiteral("report"), true).matches(entry));
    QVERIFY(!SearchPredicate::name(QStringLiteral("invoice")).matches(entry));

    // The path is not the name: a folder called "reports" must not make every
    // file under it a match for "report".
    QVERIFY(!SearchPredicate::name(QStringLiteral("a/")).matches(entry));
}

void TestSearchQuery::aNameFoldsBeyondAscii()
{
    // SQLite's own NOCASE folds ASCII and stops, which is why both engines go
    // through one folding rather than each having its own idea.
    const FileEntry entry = entryFor(QStringLiteral("mem:///ŁÓDŹ-raport.txt"));
    QVERIFY(SearchPredicate::name(QStringLiteral("łódź")).matches(entry));
    QVERIFY(!SearchPredicate::name(QStringLiteral("łódź"), true).matches(entry));
}

void TestSearchQuery::anExtensionIsExactAndCaseless()
{
    const FileEntry pdf = entryFor(QStringLiteral("mem:///a.PDF"));
    QVERIFY(SearchPredicate::extensions({ QStringLiteral("pdf") }).matches(pdf));
    QVERIFY(SearchPredicate::extensions({ QStringLiteral("PDF") }).matches(pdf));
    // Exact, not a substring: "df" is not an extension anybody meant.
    QVERIFY(!SearchPredicate::extensions({ QStringLiteral("df") }).matches(pdf));
}

void TestSearchQuery::aSizeRangeIsInclusive()
{
    const FileEntry entry = entryFor(QStringLiteral("mem:///f.bin"), 1000);
    QVERIFY(SearchPredicate::minSize(1000).matches(entry));
    QVERIFY(SearchPredicate::maxSize(1000).matches(entry));
    QVERIFY(!SearchPredicate::minSize(1001).matches(entry));
    QVERIFY(!SearchPredicate::maxSize(999).matches(entry));
}

void TestSearchQuery::aDateRangeNeedsADate()
{
    const FileEntry entry
        = entryFor(QStringLiteral("mem:///f.bin"), 0, false, QDateTime::fromSecsSinceEpoch(1000));
    QVERIFY(SearchPredicate::modifiedAfter(1000).matches(entry));
    QVERIFY(SearchPredicate::modifiedBefore(1000).matches(entry));
    QVERIFY(!SearchPredicate::modifiedAfter(1001).matches(entry));

    // A backend that does not report one is not a file from the beginning of
    // time: asked about a date, it has no answer, so it is not a match.
    const FileEntry undated = entryFor(QStringLiteral("mem:///g.bin"), 0, false, QDateTime());
    QVERIFY(!SearchPredicate::modifiedAfter(0).matches(undated));
    QVERIFY(!SearchPredicate::modifiedBefore(9999999999).matches(undated));
}

void TestSearchQuery::aKindIsOneOrTheOther()
{
    const FileEntry file = entryFor(QStringLiteral("mem:///a.txt"));
    const FileEntry folder = entryFor(QStringLiteral("mem:///a"), 0, true);

    QVERIFY(SearchPredicate::kind(false).matches(file));
    QVERIFY(!SearchPredicate::kind(true).matches(file));
    QVERIFY(SearchPredicate::kind(true).matches(folder));
    QVERIFY(!SearchPredicate::kind(false).matches(folder));
}

void TestSearchQuery::aPathIsAPrefixOfTheWholeUri()
{
    const FileEntry entry = entryFor(QStringLiteral("mem:///projects/2026/notes.md"));
    QVERIFY(SearchPredicate::underPath(QStringLiteral("mem:///projects")).matches(entry));
    QVERIFY(SearchPredicate::underPath(QStringLiteral("mem:///projects/2026")).matches(entry));
    QVERIFY(!SearchPredicate::underPath(QStringLiteral("mem:///archive")).matches(entry));
    // Scheme and authority included, because a hit has to carry enough to be
    // opened and two drives can hold the same path.
    QVERIFY(!SearchPredicate::underPath(QStringLiteral("file:///projects")).matches(entry));
}

void TestSearchQuery::aBlankCriterionIsNotACriterion()
{
    // A form left empty is not somebody asking for files with no name.
    SearchQuery query;
    query.addIfSet(SearchPredicate::name(QString()));
    query.addIfSet(SearchPredicate::extensions({ QString() }));
    query.addIfSet(SearchPredicate::underPath(QString()));
    query.addIfSet(SearchPredicate::minSize(-1));
    query.addIfSet(SearchPredicate::maxSize(-1));
    QVERIFY(query.predicates.isEmpty());

    // And nought bytes is a criterion somebody typed, not an absent one.
    query.addIfSet(SearchPredicate::minSize(0));
    QCOMPARE(query.predicates.size(), 1);
}

void TestSearchQuery::aNameCanBeAGlobOrAnExpression()
{
    const FileEntry report = entryFor(QStringLiteral("mem:///report-q1.pdf"));
    const FileEntry other = entryFor(QStringLiteral("mem:///summary.pdf"));

    QVERIFY(SearchPredicate::nameGlob(QStringLiteral("report-*.pdf")).matches(report));
    QVERIFY(!SearchPredicate::nameGlob(QStringLiteral("report-*.pdf")).matches(other));
    // Anchored, so a glob is a shape for the whole name and not a substring.
    QVERIFY(!SearchPredicate::nameGlob(QStringLiteral("report")).matches(report));

    QVERIFY(SearchPredicate::nameRegex(QStringLiteral("^report-q[0-9]")).matches(report));
    QVERIFY(!SearchPredicate::nameRegex(QStringLiteral("^report-q[0-9]")).matches(other));

    // And a mode is chosen rather than guessed at: a file really called a.b is
    // found by asking for a.b, which as an expression would match "axb" too.
    const FileEntry dotted = entryFor(QStringLiteral("mem:///a.b"));
    const FileEntry decoy = entryFor(QStringLiteral("mem:///axb"));
    QVERIFY(SearchPredicate::name(QStringLiteral("a.b")).matches(dotted));
    QVERIFY(!SearchPredicate::name(QStringLiteral("a.b")).matches(decoy));
    QVERIFY(SearchPredicate::nameRegex(QStringLiteral("a.b")).matches(decoy));
}

void TestSearchQuery::aPatternThatDoesNotCompileMatchesNothing()
{
    // A typo must not turn into a search of the whole disk.
    const FileEntry entry = entryFor(QStringLiteral("mem:///anything.txt"));
    QVERIFY(!SearchPredicate::nameRegex(QStringLiteral("[unclosed")).matches(entry));
}

void TestSearchQuery::wholeWordsStopReportMatchingReporting()
{
    const FileEntry document = entryFor(QStringLiteral("mem:///annual report 2026.pdf"));
    const FileEntry hyphenated = entryFor(QStringLiteral("mem:///annual-report-2026.pdf"));
    const FileEntry longer = entryFor(QStringLiteral("mem:///reporting-tool.py"));

    SearchPredicate word = SearchPredicate::name(QStringLiteral("report"));
    word.wholeWord = true;
    QVERIFY(word.matches(document));
    // A hyphen is a separator in a file name whatever a word boundary says
    // about prose.
    QVERIFY(word.matches(hyphenated));
    QVERIFY2(!word.matches(longer), "whole words has to stop report matching reporting");

    // And without it, the substring behaves as it always did.
    QVERIFY(SearchPredicate::name(QStringLiteral("report")).matches(longer));
}

void TestSearchQuery::notOnANameAndOnAPathTakeTheOtherHalf()
{
    const FileEntry draft = entryFor(QStringLiteral("mem:///notes/draft.txt"));
    const FileEntry final_ = entryFor(QStringLiteral("mem:///notes/final.txt"));

    SearchPredicate notDraft = SearchPredicate::name(QStringLiteral("draft"));
    notDraft.negate = true;
    QVERIFY(!notDraft.matches(draft));
    QVERIFY(notDraft.matches(final_));

    SearchPredicate notInNotes = SearchPredicate::pathContains(QStringLiteral("/notes/"));
    notInNotes.negate = true;
    QVERIFY(!notInNotes.matches(draft));
    QVERIFY(notInNotes.matches(entryFor(QStringLiteral("mem:///other/draft.txt"))));
}

void TestSearchQuery::anExtensionIsOneOfAList()
{
    const SearchPredicate photos = SearchPredicate::extensions(
        { QStringLiteral("jpg"), QStringLiteral(".JPEG"), QStringLiteral(" heic ") });

    QVERIFY(photos.matches(entryFor(QStringLiteral("mem:///a.jpg"))));
    QVERIFY2(photos.matches(entryFor(QStringLiteral("mem:///b.jpeg"))),
        "a leading dot and stray spaces are how people type a list");
    QVERIFY(photos.matches(entryFor(QStringLiteral("mem:///c.HEIC"))));
    QVERIFY(!photos.matches(entryFor(QStringLiteral("mem:///d.png"))));
}

void TestSearchQuery::aPathIsSearchedForWhereverItAppears()
{
    const FileEntry deep = entryFor(QStringLiteral("mem:///work/invoices/2026/march.pdf"));

    QVERIFY(SearchPredicate::pathContains(QStringLiteral("invoices/2026")).matches(deep));
    QVERIFY(SearchPredicate::pathContains(QStringLiteral("INVOICES")).matches(deep));
    QVERIFY(!SearchPredicate::pathContains(QStringLiteral("invoices/2025")).matches(deep));
    // Different from the name, which is the whole reason it is its own field.
    QVERIFY(!SearchPredicate::name(QStringLiteral("invoices")).matches(deep));
}

void TestSearchQuery::hiddenIsACriterionOfItsOwn()
{
    FileEntry dotfile = entryFor(QStringLiteral("mem:///.bashrc"));
    dotfile.isHidden = true;
    const FileEntry plain = entryFor(QStringLiteral("mem:///notes.txt"));

    QVERIFY(SearchPredicate::hidden(true).matches(dotfile));
    QVERIFY(!SearchPredicate::hidden(true).matches(plain));
    QVERIFY(SearchPredicate::hidden(false).matches(plain));
    QVERIFY(!SearchPredicate::hidden(false).matches(dotfile));
}

void TestSearchQuery::aTypeClassComesFromWhatIsInTheFile()
{
    // The two cases the class exists for, and which an extension cannot answer:
    // a file with no suffix at all, and one whose suffix is a lie.
    const QByteArray png = QByteArrayLiteral("\x89PNG\r\n\x1a\n") + QByteArray(64, '\0');
    const QByteArray script = QByteArrayLiteral("FROM debian:bookworm\nRUN apt-get update\n");

    const SampleReader reader = [&](const VfsUri& uri) -> QByteArray {
        return uri.fileName() == QLatin1String("Dockerfile") ? script : png;
    };

    const SearchPredicate images = SearchPredicate::typeClasses({ QStringLiteral("image") });
    const SearchPredicate code = SearchPredicate::typeClasses({ QStringLiteral("code") });

    const FileEntry mislabelled = entryFor(QStringLiteral("mem:///holiday.txt"));
    QVERIFY2(images.matches(mislabelled, reader), "a png called .txt is still a picture");
    QVERIFY(!code.matches(mislabelled, reader));

    const FileEntry dockerfile = entryFor(QStringLiteral("mem:///Dockerfile"));
    QVERIFY2(code.matches(dockerfile, reader), "a name with no suffix is what the class is for");
    QVERIFY(!images.matches(dockerfile, reader));

    // A folder is a class of its own and nothing would be learnt by reading it.
    const FileEntry folder = entryFor(QStringLiteral("mem:///archive"), 0, true);
    QVERIFY(SearchPredicate::typeClasses({ QStringLiteral("folder") }).matches(folder, reader));
    QVERIFY(!images.matches(folder, reader));
}

void TestSearchQuery::aTypeClassWithNothingToReadMatchesNothing()
{
    // The honest answer for a source that cannot look inside: not a match, and
    // certainly not everything.
    const SearchPredicate images = SearchPredicate::typeClasses({ QStringLiteral("image") });
    QVERIFY(images.needsSample());
    QVERIFY(!images.matches(entryFor(QStringLiteral("mem:///a.png"))));
}

void TestSearchQuery::datesPeopleTypeBecomeMoments()
{
    const QDateTime now = QDateTime(QDate(2026, 8, 11), QTime(14, 30));

    QCOMPARE(parseWhen(QStringLiteral("today"), now), QDateTime(QDate(2026, 8, 11), QTime(0, 0)));
    QCOMPARE(parseWhen(QStringLiteral("yesterday"), now), QDateTime(QDate(2026, 8, 10), QTime(0, 0)));
    // 11 August 2026 is a Tuesday, so this week began on the Monday.
    QCOMPARE(parseWhen(QStringLiteral("this week"), now), QDateTime(QDate(2026, 8, 10), QTime(0, 0)));
    QCOMPARE(parseWhen(QStringLiteral("this month"), now), QDateTime(QDate(2026, 8, 1), QTime(0, 0)));
    QCOMPARE(parseWhen(QStringLiteral("this year"), now), QDateTime(QDate(2026, 1, 1), QTime(0, 0)));

    QCOMPARE(parseWhen(QStringLiteral("last 7 days"), now), now.addDays(-7));
    QCOMPARE(parseWhen(QStringLiteral(">30d"), now), now.addDays(-30));
    QCOMPARE(parseWhen(QStringLiteral("24h"), now), now.addSecs(-24 * 3600));
    QCOMPARE(parseWhen(QStringLiteral("2 weeks"), now), now.addDays(-14));
    QCOMPARE(parseWhen(QStringLiteral("3 months"), now), now.addMonths(-3));
    QCOMPARE(parseWhen(QStringLiteral("  TODAY  "), now), QDateTime(QDate(2026, 8, 11), QTime(0, 0)));

    // And the day somebody knows exactly.
    QCOMPARE(parseWhen(QStringLiteral("2026-03-01"), now), QDateTime(QDate(2026, 3, 1), QTime(0, 0)));
}

void TestSearchQuery::aDateNobodyCanParseNarrowsNothingRatherThanEverything()
{
    const QDateTime now = QDateTime(QDate(2026, 8, 11), QTime(14, 30));
    for (const char* nonsense : { "", "  ", "soonish", "last fortnight", "7 parsecs", "31/02/2026" })
        QVERIFY2(!parseWhen(QLatin1String(nonsense), now).isValid(), nonsense);
}

void TestSearchQuery::createdAndAccessedAreAbsentRatherThanTheEpoch()
{
    // Most backends report a modification time and nothing else. A search for
    // files made this week must not sweep up a whole drive that cannot say.
    FileEntry unknown = entryFor(QStringLiteral("mem:///a.txt"));
    unknown.created = QDateTime();
    unknown.accessed = QDateTime();

    QVERIFY(!SearchPredicate::createdAfter(0).matches(unknown));
    QVERIFY(!SearchPredicate::createdBefore(9999999999).matches(unknown));
    QVERIFY(!SearchPredicate::accessedAfter(0).matches(unknown));

    FileEntry known = unknown;
    known.created = QDateTime::fromSecsSinceEpoch(1000);
    known.accessed = QDateTime::fromSecsSinceEpoch(2000);
    QVERIFY(SearchPredicate::createdAfter(999).matches(known));
    QVERIFY(!SearchPredicate::createdAfter(1001).matches(known));
    QVERIFY(SearchPredicate::accessedAfter(2000).matches(known));
    QVERIFY(!SearchPredicate::accessedAfter(2001).matches(known));
}

void TestSearchQuery::excludedFoldersAreMatchedAsGlobs()
{
    SearchQuery query;
    query.excluded = { QStringLiteral("node_modules"), QStringLiteral(".git"), QStringLiteral("build*") };

    QVERIFY(query.isExcluded(QStringLiteral("node_modules")));
    QVERIFY(query.isExcluded(QStringLiteral(".git")));
    QVERIFY(query.isExcluded(QStringLiteral("build")));
    QVERIFY(query.isExcluded(QStringLiteral("build-debug")));
    QVERIFY(!query.isExcluded(QStringLiteral("src")));
    QVERIFY2(!query.isExcluded(QStringLiteral("my-node_modules-notes")),
        "anchored, so a folder merely mentioning one is not one");
}

// ------------------------------------------------------------------ the planner

void TestSearchQuery::theIndexTakesWhatSqlCanState()
{
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("report")));
    query.add(SearchPredicate::extensions({ QStringLiteral("pdf") }));
    query.add(SearchPredicate::minSize(10));
    query.add(SearchPredicate::kind(false));

    const SearchPlan plan = planSearch(query, SearchSource::Index);
    QCOMPARE(plan.pushedDown().size(), 4);
    QVERIFY(plan.pushedDownEverything());
}

void TestSearchQuery::aWalkPushesNothingDownBecauseItHasNowhereToPushIt()
{
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("report")));
    query.add(SearchPredicate::minSize(10));

    // A walk lists a directory and looks at what came back. Calling that a
    // push-down would be a second name for the same evaluator.
    const SearchPlan plan = planSearch(query, SearchSource::Walk);
    QVERIFY(plan.pushedDown().isEmpty());
    QCOMPARE(plan.remainder().size(), 2);
    QVERIFY(!plan.pushedDownEverything());
}

void TestSearchQuery::whatCannotBePushedDownIsEvaluatedRatherThanDropped()
{
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("report")));
    query.add(SearchPredicate::underPath(QStringLiteral("mem:///projects")));

    const SearchPlan plan = planSearch(query, SearchSource::Index);
    // The name is a column; the path prefix is a uri no row stores whole.
    QCOMPARE(plan.pushedDown().size(), 1);
    QCOMPARE(plan.remainder().size(), 1);
    QVERIFY2(!plan.pushedDownEverything(), "the query has to admit it was not answered whole");

    // And what was left over is applied, not lost.
    QVERIFY(plan.matches(entryFor(QStringLiteral("mem:///projects/report.pdf"))));
    QVERIFY(!plan.matches(entryFor(QStringLiteral("mem:///archive/report.pdf"))));
}

void TestSearchQuery::theExpensiveCriterionIsEvaluatedLast()
{
    // Written worst-first, on purpose. Filter to the PDFs and then look inside
    // them; look inside everything and then keep the PDFs is the same answer
    // arrived at by reading the whole disk.
    SearchPredicate inside = SearchPredicate::name(QStringLiteral("invoice"));
    inside.cost = PredicateCost::Content;
    SearchPredicate header = SearchPredicate::name(QStringLiteral("canon"));
    header.cost = PredicateCost::Metadata;

    SearchQuery query;
    query.add(inside);
    query.add(header);
    query.add(SearchPredicate::underPath(QStringLiteral("mem:///photos")));
    query.add(SearchPredicate::extensions({ QStringLiteral("pdf") }));

    const SearchPlan plan = planSearch(query, SearchSource::Index);
    // The extension is first because it is not evaluated at all: it went into
    // the source's own query, which is what being first really means.
    QCOMPARE(plan.pushedDown().size(), 1);
    QCOMPARE(plan.pushedDown().first().field, SearchPredicate::Field::Extension);

    const QList<SearchPredicate> order = plan.remainder();
    QCOMPARE(order.size(), 3);
    QCOMPARE(order.at(0).field, SearchPredicate::Field::Under); // a string comparison
    QCOMPARE(order.at(1).cost, PredicateCost::Metadata); // a small read
    QCOMPARE(order.at(2).cost, PredicateCost::Content); // the whole file
}

void TestSearchQuery::criteriaOfOneCostKeepTheOrderTheyWereWrittenIn()
{
    // Somebody who puts the narrow one first is telling you something, and a
    // sort that reshuffles equals throws it away.
    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("first")));
    query.add(SearchPredicate::extensions({ QStringLiteral("pdf") }));
    query.add(SearchPredicate::minSize(10));

    const QList<SearchPredicate> order = planSearch(query, SearchSource::Walk).remainder();
    QCOMPARE(order.size(), 3);
    QCOMPARE(order.at(0).field, SearchPredicate::Field::Name);
    QCOMPARE(order.at(1).field, SearchPredicate::Field::Extension);
    QCOMPARE(order.at(2).field, SearchPredicate::Field::Size);
}

// -------------------------------------------------------------- the two engines

bool TestSearchQuery::buildFixture()
{
    const QDateTime old = QDateTime::fromSecsSinceEpoch(1000000000);
    const QDateTime recent = QDateTime::fromSecsSinceEpoch(1700000000);

    m_fs->addFile(QStringLiteral("/projects/annual-Report.pdf"), QByteArray(5000, 'x'), recent);
    m_fs->addFile(QStringLiteral("/projects/report-draft.txt"), QByteArray(10, 'x'), old);
    m_fs->addFile(QStringLiteral("/projects/ŁÓDŹ-raport.pdf"), QByteArray(2000, 'x'), recent);
    m_fs->addFile(QStringLiteral("/archive/old-report.pdf"), QByteArray(9000, 'x'), old);
    m_fs->addFile(QStringLiteral("/archive/notes.md"), QByteArray(1, 'x'), old);

    auto* scan = new ScanTask(
        m_fs, VfsUri::fromString(QStringLiteral("mem:///")), QStringLiteral("scratch"), m_index.get());
    m_tasks->submit(scan);
    if (!waitForTask(scan) || scan->state() != Task::State::Succeeded)
        return false;

    const Result<QList<IndexVolume>> volumes = m_index->volumes();
    if (!volumes.ok() || volumes.value().isEmpty())
        return false;
    m_volumeId = volumes.value().first().id;
    return true;
}

QStringList TestSearchQuery::throughTheWalk(const SearchQuery& query)
{
    QStringList uris;
    auto* task = new LiveSearchTask(m_fs, VfsUri::fromString(QStringLiteral("mem:///")), query);
    connect(task, &LiveSearchTask::hitsFound, this, [&uris](const FileEntryList& batch) {
        for (const FileEntry& entry : batch)
            uris.append(entry.uri.toString());
    });
    m_tasks->submit(task);
    if (!waitForTask(task))
        return { QStringLiteral("<the walk never finished>") };
    uris.sort();
    return uris;
}

QStringList TestSearchQuery::throughTheIndex(const SearchQuery& query)
{
    SearchQuery scoped = query;
    scoped.volumeId = m_volumeId;

    QStringList uris;
    auto* task = new IndexSearchTask(m_index.get(), scoped);
    connect(task, &IndexSearchTask::resultsReady, this, [&uris](const FileEntryList& entries) {
        for (const FileEntry& entry : entries)
            uris.append(entry.uri.toString());
    });
    m_tasks->submit(task);
    if (!waitForTask(task))
        return { QStringLiteral("<the index never answered>") };
    uris.sort();
    return uris;
}

/// The test that proves the merge lost nothing.
///
/// Every criterion that exists, asked of one tree through both engines. They
/// are allowed to differ in how fast they are and in nothing else.
void TestSearchQuery::everyCriterionAnswersTheSameThroughBothEngines()
{
    QVERIFY(buildFixture());

    struct Case
    {
        const char* what;
        SearchQuery query;
    };

    QList<Case> cases;
    {
        SearchQuery byName;
        byName.add(SearchPredicate::name(QStringLiteral("report")));
        cases.append({ "a name, folded", byName });

        SearchQuery caseSensitive;
        caseSensitive.add(SearchPredicate::name(QStringLiteral("Report"), true));
        cases.append({ "a name, exactly as typed", caseSensitive });

        SearchQuery nonAscii;
        nonAscii.add(SearchPredicate::name(QStringLiteral("łódź")));
        cases.append({ "a name beyond ascii", nonAscii });

        SearchQuery byExtension;
        byExtension.add(SearchPredicate::extensions({ QStringLiteral("PDF") }));
        cases.append({ "an extension", byExtension });

        SearchQuery bySize;
        bySize.add(SearchPredicate::minSize(2000));
        bySize.add(SearchPredicate::maxSize(8000));
        cases.append({ "a size range", bySize });

        SearchQuery filesOnly;
        filesOnly.add(SearchPredicate::kind(false));
        cases.append({ "files only", filesOnly });

        SearchQuery dirsOnly;
        dirsOnly.add(SearchPredicate::kind(true));
        cases.append({ "folders only", dirsOnly });

        SearchQuery underFolder;
        underFolder.add(SearchPredicate::underPath(QStringLiteral("mem:///projects")));
        cases.append({ "inside one folder", underFolder });

        SearchQuery together;
        together.add(SearchPredicate::name(QStringLiteral("report")));
        together.add(SearchPredicate::extensions({ QStringLiteral("pdf") }));
        together.add(SearchPredicate::underPath(QStringLiteral("mem:///projects")));
        together.add(SearchPredicate::kind(false));
        cases.append({ "all of them at once", together });
    }

    for (const Case& one : cases) {
        const QStringList walked = throughTheWalk(one.query);
        const QStringList indexed = throughTheIndex(one.query);
        QVERIFY2(walked == indexed,
            qPrintable(QStringLiteral("%1: the walk said [%2] and the index said [%3]")
                           .arg(QLatin1String(one.what), walked.join(QStringLiteral(", ")),
                               indexed.join(QStringLiteral(", ")))));
        QVERIFY2(!walked.isEmpty(),
            qPrintable(QStringLiteral("%1 matched nothing at all, so it proves nothing")
                           .arg(QLatin1String(one.what))));
    }
}

/// A date is a column the index has and no clause reads yet. The answer still
/// has to be right: the criterion is evaluated on the way out instead of being
/// quietly ignored, which is the difference between a slower answer and a wrong
/// one.
void TestSearchQuery::aCriterionTheIndexCannotStateStillNarrowsItsAnswer()
{
    QVERIFY(buildFixture());

    SearchQuery recent;
    recent.add(SearchPredicate::kind(false)); // the folders were all made just now
    recent.add(SearchPredicate::modifiedAfter(1500000000));

    // One criterion into the SQL, one left for the evaluator.
    const SearchPlan plan = planSearch(recent, SearchSource::Index);
    QCOMPARE(plan.pushedDown().size(), 1);
    QCOMPARE(plan.remainder().size(), 1);
    QVERIFY2(!plan.pushedDownEverything(), "the index claimed a criterion it has no clause for");

    const QStringList indexed = throughTheIndex(recent);
    QCOMPARE(indexed, throughTheWalk(recent));
    QCOMPARE(indexed.size(), 2);
    QVERIFY(indexed.contains(QStringLiteral("mem:///projects/annual-Report.pdf")));
    QVERIFY(indexed.contains(QStringLiteral("mem:///projects/ŁÓDŹ-raport.pdf")));
}

MOLE_TEST_MAIN(TestSearchQuery)
#include "tst_SearchQuery.moc"
