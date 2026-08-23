#include "support/MoleTestMain.h"
#include "support/SqliteFaults.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/text/DelimitedStore.h"
#include "core/text/DelimitedStreamParser.h"
#include "core/text/ImportDelimitedTask.h"
#include "core/text/ImportJsonLinesTask.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <thread>

using namespace mole;
using namespace mole::test;

/// The delimited pipeline: a file streamed in, queried out. This is what makes
/// a table with no row limit possible, so the tests are about size and shape
/// rather than about parsing niceties -- those live with the whole-file parser.
class TestDelimitedStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- the incremental parser ----
    void parsesRowsAcrossChunkBoundaries();
    void keepsQuotedNewlinesInsideOneField();
    void handlesEscapedQuotes();
    void handlesCarriageReturns();
    void returnsTheLastLineWithoutATrailingBreak();

    // ---- the store ----
    void countsAndPagesRows();
    void filtersAcrossEveryColumn();
    void filterTreatsWildcardsAsLiteralText();
    void padsRaggedRows();
    void measuresColumnWidths();

    // ---- the import ----
    void importsAFileLargerThanOneChunk();
    void widensTheTableToTheWidestRow();
    void detectsTheSeparator();

    // ---- the same store, from a file of json records ----
    void theColumnsAreTheKeysInTheOrderTheFileWritesThem();
    void aKeyFirstSeenAfterTheSampleIsCountedAndHasNoColumn();
    void aMalformedLineIsCountedAndTheImportFinishes();
    void aFileWhoseRecordsAreNotObjectsSaysSoRatherThanFailing();
    void everyKindOfJsonValueBecomesOneCell();
    void theTextOrderScanIsNotFooledByWhatIsInsideAValue();

    // ---- a reader and a writer on one file ----
    void theTableIsReadThroughoutAnImportAndEveryReadIsAnswered();
    void aReadIsAnsweredWhileAWriteIsStillInFlight();
    void aReadThatFailedIsNotATableWithNothingInIt();
    void aBatchCommitThatFailedFailsTheImportRatherThanLosingTheRows();
    void anImportWhoseLastCommitFailedSaysSoRatherThanFinishing();
    void aWriteRefusedByAnotherConnectionIsReportedAsALockedDatabase();
    void aWriteWithNoRoomLeftIsToldApartFromALockedDatabase();
    void aWriteRefusedBecauseTheDatabaseMovedOnIsNotDescribedAsAWait();
    void everyConnectionCarriesTheSettingsAndNotOnlyTheOpenersOwn();
    void noConnectionIsLeftBehindWhateverThreadTheStoreDiesOn();

private:
    ImportDelimitedTask* importFile(
        const QString& name, const std::shared_ptr<DelimitedStore>& store, QChar separator = QChar());
    ImportJsonLinesTask* importRecords(const QString& name, const std::shared_ptr<DelimitedStore>& store);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<TaskManager> m_tasks;
};

void TestDelimitedStore::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    m_tasks = std::make_unique<TaskManager>();
}

void TestDelimitedStore::cleanup()
{
    m_tasks.reset();
    m_tree.reset();
    m_dir.reset();
}

// ---------------------------------------------------------------- parser

void TestDelimitedStore::parsesRowsAcrossChunkBoundaries()
{
    DelimitedStreamParser parser(QLatin1Char(','));

    // The break lands in the middle of a field, which is exactly what a fixed
    // read size does to a real file.
    QList<QStringList> rows = parser.feed(QStringLiteral("a,b,c\n1,2"));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first(), QStringList({ "a", "b", "c" }));

    rows = parser.feed(QStringLiteral("3,4\n"));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first(), QStringList({ "1", "23", "4" }));
}

void TestDelimitedStore::keepsQuotedNewlinesInsideOneField()
{
    DelimitedStreamParser parser(QLatin1Char(','));

    // A newline inside quotes is data. Splitting on it would turn one address
    // into two broken rows.
    QList<QStringList> rows = parser.feed(QStringLiteral("name,note\n\"Smith\",\"line one\nline two\"\n"));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(1), QStringList({ "Smith", "line one\nline two" }));
}

void TestDelimitedStore::handlesEscapedQuotes()
{
    DelimitedStreamParser parser(QLatin1Char(','));
    const QList<QStringList> rows = parser.feed(QStringLiteral("\"say \"\"hi\"\"\",b\n"));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first(), QStringList({ "say \"hi\"", "b" }));
}

void TestDelimitedStore::handlesCarriageReturns()
{
    DelimitedStreamParser parser(QLatin1Char(','));

    // A Windows export must not come back with an empty row between each real
    // one, nor with a stray \r on the end of every last field.
    const QList<QStringList> rows = parser.feed(QStringLiteral("a,b\r\n1,2\r\n"));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0), QStringList({ "a", "b" }));
    QCOMPARE(rows.at(1), QStringList({ "1", "2" }));
}

void TestDelimitedStore::returnsTheLastLineWithoutATrailingBreak()
{
    DelimitedStreamParser parser(QLatin1Char(','));
    const QList<QStringList> rows = parser.feed(QStringLiteral("a,b\n1,2"));
    QCOMPARE(rows.size(), 1);

    // Plenty of files end without a newline; losing their last row would be a
    // silent, size-dependent data loss.
    QCOMPARE(parser.finish(), QStringList({ "1", "2" }));
}

// ----------------------------------------------------------------- store

void TestDelimitedStore::countsAndPagesRows()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(store->beginImport({ "id", "name" }));

    QList<QStringList> rows;
    for (int i = 0; i < 10000; ++i)
        rows.append({ QString::number(i), QStringLiteral("name %1").arg(i) });
    QVERIFY(store->addRows(rows));
    QVERIFY(store->endImport());

    QCOMPARE(store->totalRows(), 10000);

    // A window from the middle, which is all the model ever asks for.
    const QList<QStringList> window = store->rows(5000, 3);
    QCOMPARE(window.size(), 3);
    QCOMPARE(window.first().at(0), QStringLiteral("5000"));
    QCOMPARE(window.last().at(1), QStringLiteral("name 5002"));

    // Past the end is empty, not an error: a view can ask beyond the last row
    // while a filter is being applied.
    QVERIFY(store->rows(20000, 10).isEmpty());
}

void TestDelimitedStore::filtersAcrossEveryColumn()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(store->beginImport({ "city", "country" }));
    QVERIFY(store->addRows(
        { { "Kraków", "Poland" }, { "Berlin", "Germany" }, { "Poznań", "Poland" }, { "Paris", "France" } }));
    QVERIFY(store->endImport());

    QCOMPARE(store->matchingRows(QStringLiteral("Poland")), 2);
    QCOMPARE(store->matchingRows(QStringLiteral("Berlin")), 1);
    // Case-insensitive, because nobody types a filter with the right case.
    QCOMPARE(store->matchingRows(QStringLiteral("berlin")), 1);
    QCOMPARE(store->matchingRows(QStringLiteral("nowhere")), 0);

    // The whole file is searched, not the loaded part -- so a filtered window
    // starts at the first match wherever it is.
    const QList<QStringList> matches = store->rows(0, 10, QStringLiteral("Poland"));
    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches.first().at(0), QStringLiteral("Kraków"));
}

void TestDelimitedStore::filterTreatsWildcardsAsLiteralText()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(store->beginImport({ "pattern" }));
    QVERIFY(store->addRows({ { "100%" }, { "1000" }, { "a_b" }, { "axb" } }));
    QVERIFY(store->endImport());

    // A user typing "%" means a per cent sign, not "match everything". Leaving
    // LIKE's wildcards live would make the filter behave at random.
    QCOMPARE(store->matchingRows(QStringLiteral("%")), 1);
    QCOMPARE(store->matchingRows(QStringLiteral("_")), 1);
}

void TestDelimitedStore::padsRaggedRows()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(store->beginImport({ "a", "b", "c" }));

    // Real exports are ragged. Refusing them would leave the file unviewable
    // for the sake of a rule nobody agreed to.
    QVERIFY(store->addRows({ { "1" }, { "1", "2", "3", "4" } }));
    QVERIFY(store->endImport());

    const QList<QStringList> rows = store->rows(0, 10);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0), QStringList({ "1", "", "" }));
    QCOMPARE(rows.at(1), QStringList({ "1", "2", "3" }));
}

void TestDelimitedStore::measuresColumnWidths()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(store->beginImport({ "id", "description" }));
    QVERIFY(store->addRows({ { "1", "a rather long description" }, { "2", "short" } }));
    QVERIFY(store->endImport());

    const QList<int> widths = store->columnWidths();
    QCOMPARE(widths.size(), 2);
    // The header counts too: a one-character column under a longer title still
    // has to fit the title.
    QCOMPARE(widths.at(0), 2);
    QCOMPARE(widths.at(1), 25);
}

// ---------------------------------------------------------------- import

ImportDelimitedTask* TestDelimitedStore::importFile(
    const QString& name, const std::shared_ptr<DelimitedStore>& store, QChar separator)
{
    auto fs = std::make_shared<LocalFileSystem>();
    auto* task = new ImportDelimitedTask(fs, m_tree->rootUri().child(name), store);
    if (!separator.isNull())
        task->setSeparator(separator);
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 30000))
        return nullptr;
    return task;
}

ImportJsonLinesTask* TestDelimitedStore::importRecords(
    const QString& name, const std::shared_ptr<DelimitedStore>& store)
{
    auto fs = std::make_shared<LocalFileSystem>();
    auto* task = new ImportJsonLinesTask(fs, m_tree->rootUri().child(name), store);
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 30000))
        return nullptr;
    return task;
}

void TestDelimitedStore::importsAFileLargerThanOneChunk()
{
    // Over a megabyte, so the reader takes several passes and the parser has
    // to carry state across them. This is the case the old whole-file parser
    // could not serve at all.
    QByteArray contents = "id,name,value\n";
    for (int i = 0; i < 60000; ++i)
        contents += QStringLiteral("%1,name %1,value %1\n").arg(i).toUtf8();
    QVERIFY(contents.size() > 1024 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("big.csv"), contents));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportDelimitedTask* task = importFile(QStringLiteral("big.csv"), store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->importedRows(), 60000);

    QCOMPARE(store->headers(), QStringList({ "id", "name", "value" }));
    QCOMPARE(store->totalRows(), 60000);

    // Nothing was dropped at a chunk boundary: the last row is intact and so
    // is one from the middle.
    const QList<QStringList> last = store->rows(59999, 1);
    QCOMPARE(last.size(), 1);
    QCOMPARE(last.first(), QStringList({ "59999", "name 59999", "value 59999" }));

    const QList<QStringList> middle = store->rows(30000, 1);
    QCOMPARE(middle.first().at(0), QStringLiteral("30000"));

    // And a filter reaches rows far past anything a capped parser would hold.
    QCOMPARE(store->matchingRows(QStringLiteral("name 59999")), 1);
}

void TestDelimitedStore::widensTheTableToTheWidestRow()
{
    // A header that does not mention every column is common; sizing the table
    // to it alone would silently drop the extra fields.
    QVERIFY(m_tree->writeFile(QStringLiteral("ragged.csv"), QByteArray("a,b\n1,2,3\n4,5,6\n")));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());
    QVERIFY(importFile(QStringLiteral("ragged.csv"), store, QLatin1Char(',')));

    QCOMPARE(store->columnCount(), 3);
    QCOMPARE(store->rows(0, 1).first(), QStringList({ "1", "2", "3" }));
}

void TestDelimitedStore::detectsTheSeparator()
{
    QVERIFY(m_tree->writeFile(
        QStringLiteral("euro.csv"), QByteArray("name;price;qty\nwidget;1,50;3\nbolt;0,99;10\n")));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportDelimitedTask* task = importFile(QStringLiteral("euro.csv"), store);
    QVERIFY(task);

    // The semicolon splits this file consistently; the comma inside "1,50"
    // does not. Counting occurrences would pick the wrong one.
    QCOMPARE(task->separator(), QLatin1Char(';'));
    QCOMPARE(store->columnCount(), 3);
    QCOMPARE(store->rows(0, 1).first().at(1), QStringLiteral("1,50"));
}

// ------------------------------------------------- a file of json records
//
// The same store with a different parser in front of it. What is under test here
// is the shape of the table -- which columns there are, and what happens to a
// value that has no column and a line that is not a record -- because those are
// the answers a reader is silently trusting when they read the grid.

void TestDelimitedStore::theColumnsAreTheKeysInTheOrderTheFileWritesThem()
{
    // Not sorted. QJsonObject sorts its keys, so the file's own order is not
    // recoverable from the parsed value -- and the order a record was written in
    // is information about the file. Alphabetical here would be "id, when, what"
    // reordered to "id, what, when", which is why this is a case of its own.
    QVERIFY(m_tree->writeFile(QStringLiteral("events.jsonl"),
        QByteArray("{\"when\":\"09:12\",\"what\":\"opened\",\"id\":1}\n"
                   "{\"when\":\"09:14\",\"what\":\"closed\",\"id\":2}\n")));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportJsonLinesTask* task = importRecords(QStringLiteral("events.jsonl"), store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->looksLikeRecords());
    QCOMPARE(store->headers(), QStringList({ "when", "what", "id" }));
    QCOMPARE(store->totalRows(), 2);

    // A record with no trailing newline is still a record: a file that does not
    // end in one is ordinary, and dropping its last line would lose a row.
    QVERIFY(m_tree->writeFile(QStringLiteral("one.jsonl"), QByteArray("{\"only\":\"record\",\"z\":1}")));
    auto single = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("s.sqlite")));
    QVERIFY(single->open());
    ImportJsonLinesTask* alone = importRecords(QStringLiteral("one.jsonl"), single);
    QVERIFY(alone);
    QVERIFY(alone->looksLikeRecords());
    QCOMPARE(single->headers(), QStringList({ "only", "z" }));
    QCOMPARE(single->totalRows(), 1);
}

void TestDelimitedStore::aKeyFirstSeenAfterTheSampleIsCountedAndHasNoColumn()
{
    // The cost of settling the shape from the head of the file, which is the CSV
    // importer's own rule. A JSON record is free to carry a key no earlier record
    // had, so a key that turns up past the sample gets no column -- and the
    // reader is told how many values that lost rather than left to notice.
    QByteArray contents;
    while (contents.size() <= ImportJsonLinesTask::kSampleBytes) {
        contents += QStringLiteral("{\"id\":%1,\"name\":\"row\"}\n").arg(contents.size()).toUtf8();
    }
    const qint64 before = contents.count('\n');
    // Two records past the sample, each with a key nothing before them had.
    contents += QByteArray("{\"id\":1,\"name\":\"late\",\"extra\":\"unseen\"}\n");
    contents += QByteArray("{\"id\":2,\"name\":\"late\",\"extra\":\"unseen\",\"other\":7}\n");

    QVERIFY(m_tree->writeFile(QStringLiteral("late.jsonl"), contents));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportJsonLinesTask* task = importRecords(QStringLiteral("late.jsonl"), store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);

    // The columns are the sample's, and the late keys are not among them.
    QCOMPARE(store->headers(), QStringList({ "id", "name" }));
    QCOMPARE(store->totalRows(), before + 2);

    // Three values had nowhere to go: "extra" twice and "other" once.
    QCOMPARE(task->keysWithoutAColumn(), 3);

    // And the rows themselves are still there, with the columns they do have.
    QCOMPARE(store->matchingRows(QStringLiteral("late")), 2);
    QCOMPARE(store->matchingRows(QStringLiteral("unseen")), 0);
}

void TestDelimitedStore::aMalformedLineIsCountedAndTheImportFinishes()
{
    // Refusing the file outright would leave it unviewable, which is the same
    // reasoning that pads a ragged CSV row rather than rejecting it. A blank line
    // is skipped and not counted; a line that is not a record is counted.
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed.jsonl"),
        QByteArray("{\"id\":1}\n"
                   "\n"
                   "{\"id\":2,   <- not json at all\n"
                   "{\"id\":3}\n"
                   "   \n"
                   "[1,2,3]\n"
                   "42\n"
                   "{\"id\":4}\n")));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportJsonLinesTask* task = importRecords(QStringLiteral("mixed.jsonl"), store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    // Records 1, 3 and 4. Record 2 is the broken one, and its line is skipped
    // rather than taking the file down with it.
    QCOMPARE(task->importedRows(), 3);
    QCOMPARE(store->totalRows(), 3);
    // Three lines were not records: the broken object, the array and the number.
    // The two blank ones are not among them.
    QCOMPARE(task->skippedLines(), 3);
    QCOMPARE(store->matchingRows(QStringLiteral("4")), 1);
}

void TestDelimitedStore::aFileWhoseRecordsAreNotObjectsSaysSoRatherThanFailing()
{
    // A pretty-printed document under the wrong name, or a file of arrays. The
    // read succeeded and the file is not a list of records, which is a different
    // answer from an import that failed -- the viewer shows the source on it.
    QVERIFY(m_tree->writeFile(QStringLiteral("arrays.jsonl"), QByteArray("[1,2]\n[3,4]\n")));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    ImportJsonLinesTask* task = importRecords(QStringLiteral("arrays.jsonl"), store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(!task->looksLikeRecords());
    QCOMPARE(task->importedRows(), 0);
    // Nothing was imported, so the store answers nothing rather than throwing.
    QCOMPARE(store->totalRows(), 0);
    QVERIFY(store->headers().isEmpty());
}

void TestDelimitedStore::everyKindOfJsonValueBecomesOneCell()
{
    // Asked of the function rather than through a file, because this is the rule
    // the grid rests on and there are six answers to check, not one.
    const QJsonDocument parsed
        = QJsonDocument::fromJson("{\"text\":\"a string\",\"yes\":true,\"no\":false,\"num\":1.5,\"big\":1e30,"
                                  "\"nothing\":null,\"object\":{\"b\":2,\"a\":1},\"list\":[1,\"two\"]}");
    QVERIFY(parsed.isObject());
    const QJsonObject record = parsed.object();

    const QStringList headers { QStringLiteral("text"), QStringLiteral("yes"), QStringLiteral("no"),
        QStringLiteral("num"), QStringLiteral("big"), QStringLiteral("nothing"), QStringLiteral("object"),
        QStringLiteral("list"), QStringLiteral("absent") };

    qint64 unseen = 0;
    const QStringList row = jsonRecordAsRow(record, headers, &unseen);
    QCOMPARE(row.size(), headers.size());

    // A string is itself: quotes are the encoding, not the value.
    QCOMPARE(row.at(0), QStringLiteral("a string"));
    QCOMPARE(row.at(1), QStringLiteral("true"));
    QCOMPARE(row.at(2), QStringLiteral("false"));
    QCOMPARE(row.at(3), QStringLiteral("1.5"));
    // Through JSON rather than QString::number, so a large number reads back as
    // a number and not as this layer's idea of how to print a double.
    QVERIFY2(row.at(4).contains(QStringLiteral("e+30")) || row.at(4).contains(QStringLiteral("1e30")),
        qPrintable(row.at(4)));
    // The word, because empty is what an absent key gets and the two are
    // different facts about the record.
    QCOMPARE(row.at(5), QStringLiteral("null"));
    // Compact and on one line, or the grid would have a row it cannot draw.
    QCOMPARE(row.at(6), QStringLiteral("{\"a\":1,\"b\":2}"));
    QCOMPARE(row.at(7), QStringLiteral("[1,\"two\"]"));
    QCOMPARE(row.at(8), QString());
    QCOMPARE(unseen, 0);

    // And a record with more keys than columns says how many were left out.
    unseen = 0;
    jsonRecordAsRow(record, QStringList { QStringLiteral("text") }, &unseen);
    QCOMPARE(unseen, record.size() - 1);
}

void TestDelimitedStore::theTextOrderScanIsNotFooledByWhatIsInsideAValue()
{
    // The scanner exists only to order keys the parser has already confirmed, so
    // its worst case is a wrong order rather than a wrong column -- but the cases
    // that would get it wrong are exactly the ones a real file has: a colon and a
    // brace inside a string, an escaped quote, and a nested object with keys of
    // its own.
    const QByteArray line = "{\"url\":\"https://example.invalid/a:b\",\"said\":\"a \\\"quote\\\" and a {\","
                            "\"inner\":{\"zzz\":1,\"aaa\":2},\"last\":[{\"nope\":1}]}";
    const QJsonDocument parsed = QJsonDocument::fromJson(line);
    QVERIFY2(parsed.isObject(), "the fixture itself has to be valid JSON");

    QCOMPARE(jsonKeysInTextOrder(QString::fromUtf8(line), parsed.object()),
        QStringList({ "url", "said", "inner", "last" }));

    // A line the scanner cannot read at all still yields every key, just sorted:
    // it loses order and never a column.
    QCOMPARE(
        jsonKeysInTextOrder(QString(), parsed.object()), QStringList({ "inner", "last", "said", "url" }));
}

// ---------------------------------------- a reader and a writer on one file
//
// The grid is attached to the store before the import starts, deliberately, so
// the first records are on screen while the rest are read. That makes the
// interface a reader on a database an import is writing to from a pool thread,
// and these three cases are what has to hold for that to be true. Before
// MOLE-289 none of them did: the settings that let a reader and a writer share
// a file were applied to the wrong connection, so the window froze behind the
// importer and then read as an empty file.

void TestDelimitedStore::theTableIsReadThroughoutAnImportAndEveryReadIsAnswered()
{
    // Two hundred thousand records, because the subject is a long import: the
    // store commits every two thousand rows and the task reports every five
    // thousand, so the reads below take a filling table forty times over.
    constexpr qint64 kRecords = 200000;
    constexpr int kWindow = 50;

    QByteArray contents;
    contents.reserve(kRecords * 40);
    for (qint64 i = 0; i < kRecords; ++i)
        contents += QStringLiteral("{\"id\":%1,\"name\":\"name %1\"}\n").arg(i).toUtf8();
    QVERIFY(m_tree->writeFile(QStringLiteral("records.jsonl"), contents));

    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    // Not importRecords(), which waits for the task. Waiting is the one thing
    // this case must not do: the store is read while the import writes to it,
    // which is what attaching the grid before the import means.
    auto fs = std::make_shared<LocalFileSystem>();
    auto* task = new ImportJsonLinesTask(fs, m_tree->rootUri().child(QStringLiteral("records.jsonl")), store);

    // The reads the interface makes, at the cadence it makes them. The preview
    // controller refreshes the model on rowsImported, and a refresh is a count
    // and a window -- so this is one of each per batch, on this thread, against
    // a database a pool thread is writing to.
    //
    // Deliberately not a loop over the two of them. A reader always on the file
    // starves the writer instead of racing it, which is the mistake the index's
    // version of this test records: measured here, a CREATE TABLE that takes no
    // time at all sat for a second waiting for a moment with no reader in it.
    // And it would be measuring something the application never does.
    qint64 unanswered = 0;
    qint64 shortWindows = 0;
    qint64 readsOfAPartlyImportedTable = 0;
    QObject onThisThread;
    connect(task, &ImportJsonLinesTask::rowsImported, &onThisThread, [&](qint64) {
        bool readable = true;
        const qint64 count = store->matchingRows({});
        const QList<QStringList> window = store->rows(0, kWindow, {}, &readable);

        if (count < 0 || !readable) {
            ++unanswered;
        } else if (window.size() < qMin<qint64>(kWindow, count)) {
            // The count was taken first and rows only ever arrive, so a window
            // shorter than the count allows for is a read that stopped early.
            ++shortWindows;
        } else if (count > 0 && count < kRecords) {
            // The condition rather than a clock: a count between nothing and the
            // whole file is proof this read was taken while the import was still
            // running, whatever either thread happened to be scheduled to do.
            ++readsOfAPartlyImportedTable;
        }
    });

    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 60000));

    QCOMPARE(unanswered, qint64(0));
    QCOMPARE(shortWindows, qint64(0));
    QVERIFY2(readsOfAPartlyImportedTable > 0,
        "every read saw an empty table or a finished one, so nothing was read during the import "
        "and this case asserts nothing");

    // And the import that was read throughout is the whole file, rather than one
    // cut short by a write that gave up partway.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->importedRows(), kRecords);
    QCOMPARE(store->totalRows(), kRecords);
}

void TestDelimitedStore::aReadIsAnsweredWhileAWriteIsStillInFlight()
{
    // The freeze, made a fact rather than a race: the file is held exclusively
    // and the read has to be answered anyway, from what is committed, rather
    // than by waiting for a commit this test is not going to make until it has
    // the answer.
    //
    // Exclusively, and not merely under a write transaction, because that is the
    // moment the interface actually met. Under a rollback journal a writer takes
    // the file only when it commits -- and the importer commits every two
    // thousand rows, with synchronous writes, so those moments were most of the
    // import. Held still here instead of raced for.
    //
    // What the fault did is worth naming, because it is not quite what the
    // title says: Qt's SQLite driver sets a busy timeout of its own, so the read
    // did not fail, it *stalled* -- five seconds of a window that has stopped
    // answering -- and came back empty only after that. Which is why this case
    // asserts the answer and not the time it took: in WAL the read takes its own
    // snapshot and returns while the writer still holds the file, and without it
    // the read cannot be answered at all until the writer lets go, so it gives
    // up. One is a number, the other is a failure, and neither is a clock.
    QVERIFY(m_tree->writeFile(QStringLiteral("two.csv"), QByteArray("a,b\n1,2\n3,4\n")));

    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    auto store = std::make_shared<DelimitedStore>(path);
    QVERIFY(store->open());
    QVERIFY(importFile(QStringLiteral("two.csv"), store, QLatin1Char(',')));

    qint64 before = -2;
    qint64 during = -2;

    const QString side = QStringLiteral("mole-test-writer");
    // Scoped, so no copy of the connection is alive when it is removed below --
    // removeDatabase says so loudly, and a warning in a passing test is a line
    // somebody has to read and dismiss on every run.
    {
        QSqlDatabase writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), side);
        writer.setDatabaseName(path);
        QVERIFY(writer.open());
        QSqlQuery write(writer);

        std::atomic_bool connected { false };
        std::atomic_bool writeInFlight { false };

        // The reader takes its connection before the writer holds anything, because
        // a connection is made on first use and making one is itself a read of the
        // file -- doing it under the lock would be measuring the wrong thing.
        std::thread reader([store, &connected, &writeInFlight, &before, &during] {
            before = store->matchingRows({});
            connected = true;
            while (!writeInFlight.load())
                std::this_thread::yield();
            during = store->matchingRows({});
        });

        while (!connected.load())
            std::this_thread::yield();

        QVERIFY2(write.exec(QStringLiteral("BEGIN EXCLUSIVE")), qPrintable(write.lastError().text()));
        QVERIFY2(write.exec(QStringLiteral("INSERT INTO rows_ (c0, c1) VALUES ('5', '6')")),
            qPrintable(write.lastError().text()));
        writeInFlight = true;

        // Joined before the commit, so the write is in flight for the whole of the
        // read: there is no ordering here to get lucky with.
        reader.join();
        QVERIFY2(write.exec(QStringLiteral("COMMIT")), qPrintable(write.lastError().text()));
        writer.close();
    }
    QSqlDatabase::removeDatabase(side);

    QCOMPARE(before, qint64(2));
    // Two, and not three: the answer is the committed table, which is what a
    // snapshot means. -1 here is the read having given up on a locked file.
    QCOMPARE(during, qint64(2));
    QCOMPARE(store->matchingRows({}), qint64(3));
}

void TestDelimitedStore::aReadThatFailedIsNotATableWithNothingInIt()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("two.csv"), QByteArray("a,b\n1,2\n3,4\n")));

    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    auto store = std::make_shared<DelimitedStore>(path);
    QVERIFY(store->open());
    QVERIFY(importFile(QStringLiteral("two.csv"), store, QLatin1Char(',')));
    QCOMPARE(store->matchingRows({}), qint64(2));

    // The table taken out from under the store, on a connection of its own. Any
    // failure would do and this one is a fact rather than a timing, but what the
    // store meets is what it met when the importer held the file: a query that
    // does not execute.
    const QString side = QStringLiteral("mole-test-side");
    {
        QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), side);
        other.setDatabaseName(path);
        QVERIFY(other.open());
        QSqlQuery drop(other);
        QVERIFY2(drop.exec(QStringLiteral("DROP TABLE rows_")), qPrintable(drop.lastError().text()));
        other.close();
    }
    QSqlDatabase::removeDatabase(side);

    // Not nought. Nought is a file with nothing in it; this one has two rows
    // that cannot be read at the moment, and ADR-0030 settled which of those two
    // answers to give. -1 is the one the model already shows as a blank.
    QCOMPARE(store->matchingRows({}), qint64(-1));
    QCOMPARE(store->totalRows(), qint64(-1));
    // Asked again it still says not known, rather than having settled on the
    // nought that made a file still importing read as an empty one.
    QCOMPARE(store->totalRows(), qint64(-1));

    bool readable = true;
    const QList<QStringList> window = store->rows(0, 10, {}, &readable);
    QVERIFY(window.isEmpty());
    QVERIFY2(!readable, "an unreadable window came back as a window that held nothing");
}

/// The store's own connection on this thread, found rather than injected.
///
/// Qt keeps a registry of connections and this one is the connection on this file:
/// no seam in the store, no fault injector, and nothing about the store's own
/// naming assumed. It lives in tests/support/SqliteFaults.h now, because
/// everything MOLE-293 built stands on it.
static QSqlDatabase storeConnectionOn(const QString& path)
{
    return sqlite::connectionOn(path);
}

void TestDelimitedStore::aBatchCommitThatFailedFailsTheImportRatherThanLosingTheRows()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    DelimitedStore store(path);
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ QStringLiteral("id"), QStringLiteral("name") }));

    // The transaction the import opened, closed out from under it on the very
    // connection it writes through. Any commit failure would do -- what is under
    // test is what the store does with one -- and this one is a fact rather than a
    // timing: with no transaction open, SQLite refuses the commit outright.
    //
    // Arranged this way because a commit is hard to fail on purpose from outside:
    // SQLite takes the write lock at the first write and not at the commit, so
    // holding the file from another connection fails an INSERT instead, which is
    // the path that was already checked. See MOLE-291 and the note there on what a
    // fault injector would buy.
    QSqlDatabase writing = storeConnectionOn(path);
    QVERIFY2(writing.isValid(), "the store's connection has to be findable, or this case tests nothing");
    QVERIFY(writing.commit());

    // Enough rows to cross a batch boundary, which is where the commit lives.
    QList<QStringList> rows;
    rows.reserve(2500);
    for (int i = 0; i < 2500; ++i)
        rows.append({ QString::number(i), QStringLiteral("name %1").arg(i) });

    QString error;
    QVERIFY2(
        !store.addRows(rows, &error), "a batch whose commit failed was reported as a batch that was written");
    QVERIFY2(!error.isEmpty(), "a failed commit has to say something, or nothing reports the loss");

    // And the import is not finished off as though it had worked: the viewer
    // shows the failure rather than a short table presented as a whole one.
    QVERIFY(!store.endImport(&error) || !error.isEmpty());
}

void TestDelimitedStore::anImportWhoseLastCommitFailedSaysSoRatherThanFinishing()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    DelimitedStore store(path);
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ QStringLiteral("id") }));
    QVERIFY(store.addRows({ { QStringLiteral("1") }, { QStringLiteral("2") } }));

    // The same fact, at the other write site: the final commit has nothing to
    // commit, so it fails and has to report it.
    //
    // This case passes on the code before MOLE-291 as well, and that is said out
    // loud rather than left for somebody to discover: endImport() already checked
    // its commit, and what changed is the *wording* -- describe() rather than the
    // driver's text, so a locked database is reported as one. That difference only
    // shows for SQLITE_BUSY, which means a write that waits out the five-second
    // busy timeout, and nothing here can produce one at a commit. So the
    // reporting path is covered and the wording is not; see the note on MOLE-291
    // and the ticket for a fault injector.
    QSqlDatabase writing = storeConnectionOn(path);
    QVERIFY(writing.isValid());
    QVERIFY(writing.commit());

    QString error;
    QVERIFY2(!store.endImport(&error), "an import whose last commit failed was reported as finished");
    QVERIFY(!error.isEmpty());

    // The rows that were committed are still readable -- a failed commit is not a
    // corrupt file -- and the store still answers for what it holds.
    QCOMPARE(store.totalRows(), qint64(2));
}

void TestDelimitedStore::aWriteRefusedByAnotherConnectionIsReportedAsALockedDatabase()
{
    // The case MOLE-291 left open, and could not write: describe()'s wording for a
    // locked database had never been reached by anything, because reaching it means
    // a write that has waited out a five-second busy timeout. A connection told to
    // stop waiting fails at once with the same SQLITE_BUSY the timeout would have
    // produced, which is what makes this a test rather than a five-second sleep.
    // See MOLE-293, and tests/support/SqliteFaults.h for why this is SQLite's own
    // levers rather than a driver wrapper.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    DelimitedStore store(path);
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ QStringLiteral("id"), QStringLiteral("name") }));

    QSqlDatabase writing = storeConnectionOn(path);
    QVERIFY2(writing.isValid(), "the store's connection has to be findable, or this case tests nothing");
    QVERIFY(sqlite::stopWaitingForLocks(writing));

    sqlite::WriteLock held(path);
    QVERIFY2(held.isHeld(), "the lock has to be taken, or the write below is unhindered");

    QString error;
    QVERIFY2(!store.addRows({ { QStringLiteral("1"), QStringLiteral("one") } }, &error),
        "a write into a locked database was reported as a write that landed");

    // The wording, which is the whole point: a locked database is another
    // connection holding the file, and an import that reported it as a fault of
    // the disk sent whoever read it looking in the wrong place entirely.
    QVERIFY2(error.contains(QStringLiteral("locked by another connection")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("disk is full")), qPrintable(error));
}

void TestDelimitedStore::aWriteWithNoRoomLeftIsToldApartFromALockedDatabase()
{
    // The other failure that loses an import's rows, and the one whose reporting a
    // reader is most likely to meet. SQLite's own answer for it, from a database
    // capped at three pages -- so this is "database or disk is full" out of the
    // layer that really produces it, rather than a string a test wrote.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    DelimitedStore store(path);
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ QStringLiteral("id"), QStringLiteral("name") }));

    QSqlDatabase writing = storeConnectionOn(path);
    QVERIFY(writing.isValid());
    QVERIFY2(sqlite::capAtCurrentSize(writing), "the cap has to take, or the rows below are unhindered");

    QList<QStringList> rows;
    rows.reserve(4000);
    for (int i = 0; i < 4000; ++i)
        rows.append({ QString::number(i), QString(200, QLatin1Char('x')) });

    QString error;
    QVERIFY2(!store.addRows(rows, &error), "rows that did not fit were reported as rows that did");

    // Told apart from a locked database, which is what describe() exists for: this
    // one is the disk, and it says so in SQLite's words rather than in the sentence
    // about another connection.
    QVERIFY2(error.contains(QStringLiteral("disk is full")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("locked by another connection")), qPrintable(error));
}

void TestDelimitedStore::aWriteRefusedBecauseTheDatabaseMovedOnIsNotDescribedAsAWait()
{
    // Qt reports SQLite's extended code wherever there is one, and describe() used
    // to compare the whole of it against "5" and "6" -- so every busy code but the
    // bare one arrived as the driver's "database is locked Unable to fetch row".
    // 517 is SQLITE_BUSY_SNAPSHOT: another connection wrote while this one had a
    // read open, so what it is looking at is no longer the database. Nothing
    // waited and nothing is holding the file, which is why "still locked, after
    // five seconds" is the wrong thing to tell anybody about it -- being told to
    // wait is watching a progress bar that will never move. See MOLE-306.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite"));
    DelimitedStore store(path);
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ QStringLiteral("id"), QStringLiteral("name") }));

    QSqlDatabase writing = storeConnectionOn(path);
    QVERIFY(writing.isValid());
    // Inside the transaction the import already opened, which is the state this
    // happens in.
    sqlite::StaleReadSnapshot stale(writing, path);
    QVERIFY2(stale.isStale(), "the snapshot has to go stale, or the write below is unhindered");

    QString error;
    QVERIFY2(!store.addRows({ { QStringLiteral("1"), QStringLiteral("one") } }, &error),
        "a write from a stale snapshot was reported as a write that landed");

    QVERIFY2(
        error.contains(QStringLiteral("changed the database while this one was reading")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("still locked")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("seconds")), qPrintable(error));
    // And not the driver's own words, which is what arrived before.
    QVERIFY2(!error.contains(QStringLiteral("Unable to fetch row")), qPrintable(error));
}

void TestDelimitedStore::everyConnectionCarriesTheSettingsAndNotOnlyTheOpenersOwn()
{
    auto store = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store->open());

    QCOMPARE(store->pragmaValue(QStringLiteral("journal_mode")).toLower(), QStringLiteral("wal"));
    QCOMPARE(store->pragmaValue(QStringLiteral("busy_timeout")).toInt(), 5000);

    // The half that was wrong, and why this is a case of its own. A connection
    // is keyed by the thread that asked for it, so pragmas run by open() land on
    // whichever thread called open() -- the test's here, the one that draws in
    // the application. The importer's connection is made on a pool thread, and
    // it was getting SQLite's defaults: a journal per transaction, synchronous
    // writes, and no busy timeout at all.
    QString journal;
    int timeout = -1;
    std::thread elsewhere([store, &journal, &timeout] {
        journal = store->pragmaValue(QStringLiteral("journal_mode")).toLower();
        timeout = store->pragmaValue(QStringLiteral("busy_timeout")).toInt();
    });
    elsewhere.join();

    QCOMPARE(journal, QStringLiteral("wal"));
    QCOMPARE(timeout, 5000);
}

void TestDelimitedStore::noConnectionIsLeftBehindWhateverThreadTheStoreDiesOn()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("two.csv"), QByteArray("a,b\n1,2\n3,4\n")));

    // Compared against what was there before rather than against nothing, so the
    // case says something about this store and not about the whole binary.
    QStringList before = QSqlDatabase::connectionNames();
    before.sort();

    {
        auto store
            = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("a.sqlite")));
        QVERIFY(store->open());
        QVERIFY(importFile(QStringLiteral("two.csv"), store, QLatin1Char(',')));

        // Two by now: this thread's, from open(), and the importer's, made on
        // whichever pool thread ran the task.
        QVERIFY2(QSqlDatabase::connectionNames().size() >= before.size() + 2,
            "the import has to have opened a connection of its own, or this asserts nothing");
    }

    QStringList after = QSqlDatabase::connectionNames();
    after.sort();
    // close() used to remove the calling thread's connection alone, so the
    // importer's was left behind for the life of the process -- one per file
    // opened, each holding a database Mole had finished with.
    QCOMPARE(after, before);

    // And the other way round, which is what a reader moving to the next file
    // does: the interface lets go first, so the last reference is dropped by the
    // task's thread and the store is destroyed there. The connection this thread
    // opened has to go with it.
    {
        auto store
            = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("b.sqlite")));
        QVERIFY(store->open());
        QVERIFY(importFile(QStringLiteral("two.csv"), store, QLatin1Char(',')));

        std::thread elsewhere([last = std::move(store)]() mutable { last.reset(); });
        elsewhere.join();
    }

    after = QSqlDatabase::connectionNames();
    after.sort();
    QCOMPARE(after, before);
}

MOLE_TEST_MAIN(TestDelimitedStore)
#include "tst_DelimitedStore.moc"
