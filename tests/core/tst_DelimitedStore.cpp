#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/text/DelimitedStore.h"
#include "core/text/DelimitedStreamParser.h"
#include "core/text/ImportDelimitedTask.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QTemporaryDir>

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

private:
    ImportDelimitedTask* importFile(const QString& name, DelimitedStore* store, QChar separator = QChar());

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
    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ "id", "name" }));

    QList<QStringList> rows;
    for (int i = 0; i < 10000; ++i)
        rows.append({ QString::number(i), QStringLiteral("name %1").arg(i) });
    QVERIFY(store.addRows(rows));
    QVERIFY(store.endImport());

    QCOMPARE(store.totalRows(), 10000);

    // A window from the middle, which is all the model ever asks for.
    const QList<QStringList> window = store.rows(5000, 3);
    QCOMPARE(window.size(), 3);
    QCOMPARE(window.first().at(0), QStringLiteral("5000"));
    QCOMPARE(window.last().at(1), QStringLiteral("name 5002"));

    // Past the end is empty, not an error: a view can ask beyond the last row
    // while a filter is being applied.
    QVERIFY(store.rows(20000, 10).isEmpty());
}

void TestDelimitedStore::filtersAcrossEveryColumn()
{
    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ "city", "country" }));
    QVERIFY(store.addRows(
        { { "Kraków", "Poland" }, { "Berlin", "Germany" }, { "Poznań", "Poland" }, { "Paris", "France" } }));
    QVERIFY(store.endImport());

    QCOMPARE(store.matchingRows(QStringLiteral("Poland")), 2);
    QCOMPARE(store.matchingRows(QStringLiteral("Berlin")), 1);
    // Case-insensitive, because nobody types a filter with the right case.
    QCOMPARE(store.matchingRows(QStringLiteral("berlin")), 1);
    QCOMPARE(store.matchingRows(QStringLiteral("nowhere")), 0);

    // The whole file is searched, not the loaded part -- so a filtered window
    // starts at the first match wherever it is.
    const QList<QStringList> matches = store.rows(0, 10, QStringLiteral("Poland"));
    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches.first().at(0), QStringLiteral("Kraków"));
}

void TestDelimitedStore::filterTreatsWildcardsAsLiteralText()
{
    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ "pattern" }));
    QVERIFY(store.addRows({ { "100%" }, { "1000" }, { "a_b" }, { "axb" } }));
    QVERIFY(store.endImport());

    // A user typing "%" means a per cent sign, not "match everything". Leaving
    // LIKE's wildcards live would make the filter behave at random.
    QCOMPARE(store.matchingRows(QStringLiteral("%")), 1);
    QCOMPARE(store.matchingRows(QStringLiteral("_")), 1);
}

void TestDelimitedStore::padsRaggedRows()
{
    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ "a", "b", "c" }));

    // Real exports are ragged. Refusing them would leave the file unviewable
    // for the sake of a rule nobody agreed to.
    QVERIFY(store.addRows({ { "1" }, { "1", "2", "3", "4" } }));
    QVERIFY(store.endImport());

    const QList<QStringList> rows = store.rows(0, 10);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0), QStringList({ "1", "", "" }));
    QCOMPARE(rows.at(1), QStringList({ "1", "2", "3" }));
}

void TestDelimitedStore::measuresColumnWidths()
{
    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.beginImport({ "id", "description" }));
    QVERIFY(store.addRows({ { "1", "a rather long description" }, { "2", "short" } }));
    QVERIFY(store.endImport());

    const QList<int> widths = store.columnWidths();
    QCOMPARE(widths.size(), 2);
    // The header counts too: a one-character column under a longer title still
    // has to fit the title.
    QCOMPARE(widths.at(0), 2);
    QCOMPARE(widths.at(1), 25);
}

// ---------------------------------------------------------------- import

ImportDelimitedTask* TestDelimitedStore::importFile(
    const QString& name, DelimitedStore* store, QChar separator)
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

    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());

    ImportDelimitedTask* task = importFile(QStringLiteral("big.csv"), &store);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->importedRows(), 60000);

    QCOMPARE(store.headers(), QStringList({ "id", "name", "value" }));
    QCOMPARE(store.totalRows(), 60000);

    // Nothing was dropped at a chunk boundary: the last row is intact and so
    // is one from the middle.
    const QList<QStringList> last = store.rows(59999, 1);
    QCOMPARE(last.size(), 1);
    QCOMPARE(last.first(), QStringList({ "59999", "name 59999", "value 59999" }));

    const QList<QStringList> middle = store.rows(30000, 1);
    QCOMPARE(middle.first().at(0), QStringLiteral("30000"));

    // And a filter reaches rows far past anything a capped parser would hold.
    QCOMPARE(store.matchingRows(QStringLiteral("name 59999")), 1);
}

void TestDelimitedStore::widensTheTableToTheWidestRow()
{
    // A header that does not mention every column is common; sizing the table
    // to it alone would silently drop the extra fields.
    QVERIFY(m_tree->writeFile(QStringLiteral("ragged.csv"), QByteArray("a,b\n1,2,3\n4,5,6\n")));

    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());
    QVERIFY(importFile(QStringLiteral("ragged.csv"), &store, QLatin1Char(',')));

    QCOMPARE(store.columnCount(), 3);
    QCOMPARE(store.rows(0, 1).first(), QStringList({ "1", "2", "3" }));
}

void TestDelimitedStore::detectsTheSeparator()
{
    QVERIFY(m_tree->writeFile(
        QStringLiteral("euro.csv"), QByteArray("name;price;qty\nwidget;1,50;3\nbolt;0,99;10\n")));

    DelimitedStore store(QDir(m_dir->path()).filePath(QStringLiteral("t.sqlite")));
    QVERIFY(store.open());

    ImportDelimitedTask* task = importFile(QStringLiteral("euro.csv"), &store);
    QVERIFY(task);

    // The semicolon splits this file consistently; the comma inside "1,50"
    // does not. Counting occurrences would pick the wrong one.
    QCOMPARE(task->separator(), QLatin1Char(';'));
    QCOMPARE(store.columnCount(), 3);
    QCOMPARE(store.rows(0, 1).first().at(1), QStringLiteral("1,50"));
}

MOLE_TEST_MAIN(TestDelimitedStore)
#include "tst_DelimitedStore.moc"
