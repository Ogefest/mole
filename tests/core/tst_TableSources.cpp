// Arrow first, before anything from Qt: it declares a parameter called
// `signals`, and Qt's macro of that name expands to `public:`.
#ifdef MOLE_HAVE_PARQUET
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#endif

#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/data/ParquetTable.h"
#include "core/data/SqliteTable.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The tabular sources behind the grid. Both answer the same interface, so the
/// tests are about what each one has to get right on its own.
class TestTableSources : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- SQLite ----
    void listsTablesAndViews();
    void openingADatabaseCountsNothing();
    void readsTheSelectedTable();
    void switchingTablesChangesTheShape();
    void filtersAcrossColumnsIncludingNumbers();
    void distinguishesNullFromEmpty();
    void survivesAwkwardIdentifiers();
    void refusesSomethingThatIsNotADatabase();
    void opensReadOnly();

    // ---- Parquet ----
    void readsAParquetFile();
    void pagesThroughAParquetFileWithoutReadingItAll();
    void parquetReportsItselfWhenUnsupported();

private:
    QString databasePath() const;
    void buildDatabase();

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestTableSources::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestTableSources::cleanup()
{
    m_dir.reset();
}

QString TestTableSources::databasePath() const
{
    return QDir(m_dir->path()).filePath(QStringLiteral("test.sqlite"));
}

void TestTableSources::buildDatabase()
{
    const QString name = QStringLiteral("builder");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(databasePath());
        QVERIFY(db.open());

        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE people (id INTEGER PRIMARY KEY, "
                                          "name TEXT, city TEXT, age INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO people VALUES (1,'Ada','Kraków',36)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO people VALUES (2,'Grace','Berlin',45)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO people VALUES (3,'Alan',NULL,41)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO people VALUES (4,'Edsger','',38)")));

        // A second table with a different shape, and an awkward name.
        QVERIFY(query.exec(QStringLiteral(R"(CREATE TABLE "order" (ref TEXT))")));
        QVERIFY(query.exec(QStringLiteral(R"(INSERT INTO "order" VALUES ('A-1'))")));

        QVERIFY(query.exec(QStringLiteral("CREATE VIEW adults AS SELECT name FROM people "
                                          "WHERE age >= 40")));
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}

// ---------------------------------------------------------------- SQLite

void TestTableSources::listsTablesAndViews()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QString error;
    QVERIFY2(table.open(&error), qPrintable(error));

    // Views count: from the outside both are something to look at, and leaving
    // views out would hide half of some databases.
    QCOMPARE(table.tableNames(), QStringList({ "adults", "order", "people" }));
    QCOMPARE(table.rowCountOf(QStringLiteral("people")), 4);
    QCOMPARE(table.rowCountOf(QStringLiteral("adults")), 2);
}

void TestTableSources::openingADatabaseCountsNothing()
{
    // Opening used to count every table and view in the file before anything
    // was drawn, and a COUNT(*) is a walk of the table however it is indexed --
    // so a database of a few large tables held the window for as long as it took
    // to walk all of them. The names are cheap and are answered at once; the
    // counts are expensive and are somebody else's job, on another thread.
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());

    for (const QString& name : table.tableNames()) {
        QCOMPARE(table.knownRowCountOf(name), SqliteTable::kRowsNotCounted);
    }
    // Including the table it selected on the way in, which the grid reads.
    QCOMPARE(table.totalRows(), SqliteTable::kRowsNotCounted);
    QCOMPARE(table.matchingRows({}), SqliteTable::kRowsNotCounted);

    // A count taken once is remembered for the life of the open file: the
    // picker asks for every name and the summary strip asks again for the
    // current one, and the file cannot change under a read-only connection.
    QCOMPARE(table.rowCountOf(QStringLiteral("people")), 4);
    QCOMPARE(table.knownRowCountOf(QStringLiteral("people")), 4);

    // Or handed in from the connection the counting task walked it on.
    table.setRowCount(QStringLiteral("adults"), 2);
    QCOMPARE(table.knownRowCountOf(QStringLiteral("adults")), 2);
}

void TestTableSources::readsTheSelectedTable()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());
    QVERIFY(table.setCurrentTable(QStringLiteral("people")));

    QCOMPARE(table.headers(), QStringList({ "id", "name", "city", "age" }));
    // Once somebody has counted it. Until then the size of the table is not
    // known -- see openingADatabaseCountsNothing().
    QCOMPARE(table.rowCountOf(QStringLiteral("people")), 4);
    QCOMPARE(table.totalRows(), 4);

    const QList<QStringList> window = table.rows(1, 2);
    QCOMPARE(window.size(), 2);
    QCOMPARE(window.first().at(1), QStringLiteral("Grace"));
    QCOMPARE(window.last().at(1), QStringLiteral("Alan"));

    // Widths measured from the contents, header included.
    const QList<int> widths = table.columnWidths();
    QCOMPARE(widths.size(), 4);
    QCOMPARE(widths.at(1), 6); // "Edsger"
}

void TestTableSources::switchingTablesChangesTheShape()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());

    QVERIFY(table.setCurrentTable(QStringLiteral("people")));
    QCOMPARE(table.columnCount(), 4);

    QVERIFY(table.setCurrentTable(QStringLiteral("order")));
    QCOMPARE(table.columnCount(), 1);
    QCOMPARE(table.headers(), QStringList { QStringLiteral("ref") });
    QCOMPARE(table.rowCountOf(QStringLiteral("order")), 1);
    QCOMPARE(table.totalRows(), 1);

    // A table that is not there is refused rather than leaving the old one
    // selected while claiming otherwise.
    QVERIFY(!table.setCurrentTable(QStringLiteral("nope")));
}

void TestTableSources::filtersAcrossColumnsIncludingNumbers()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());
    QVERIFY(table.setCurrentTable(QStringLiteral("people")));

    QCOMPARE(table.matchingRows(QStringLiteral("Kraków")), 1);
    // Case-insensitive, because nobody types a filter with the right case.
    QCOMPARE(table.matchingRows(QStringLiteral("berlin")), 1);
    // Numbers are cast, or a filter would silently never match a numeric column.
    QCOMPARE(table.matchingRows(QStringLiteral("45")), 1);
    QCOMPARE(table.matchingRows(QStringLiteral("nowhere")), 0);

    // Wildcards are literal text: someone typing "%" means a per cent sign.
    QCOMPARE(table.matchingRows(QStringLiteral("%")), 0);

    const QList<QStringList> matches = table.rows(0, 10, QStringLiteral("Berlin"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().at(1), QStringLiteral("Grace"));
}

void TestTableSources::distinguishesNullFromEmpty()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());
    QVERIFY(table.setCurrentTable(QStringLiteral("people")));

    const QList<QStringList> all = table.rows(0, 10);
    QCOMPARE(all.size(), 4);

    // Alan's city is NULL; Edsger's is an empty string. Showing them alike hides
    // the difference the reader is usually looking for.
    QCOMPARE(all.at(2).at(2), QStringLiteral("NULL"));
    QCOMPARE(all.at(3).at(2), QString());
}

void TestTableSources::survivesAwkwardIdentifiers()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());

    // "order" is a keyword. Unquoted it is a syntax error, and building the
    // query by concatenation without quoting is how that becomes an injection.
    QVERIFY(table.setCurrentTable(QStringLiteral("order")));
    QCOMPARE(table.rows(0, 5).size(), 1);
    QCOMPARE(table.matchingRows(QStringLiteral("A-1")), 1);
}

void TestTableSources::refusesSomethingThatIsNotADatabase()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("notes.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a database");
    file.close();

    SqliteTable table(path);
    QString error;
    QVERIFY2(!table.open(&error), "a text file must not open as a database");
    QVERIFY(!error.isEmpty());
}

void TestTableSources::opensReadOnly()
{
    buildDatabase();
    SqliteTable table(databasePath());
    QVERIFY(table.open());
    QVERIFY(table.setCurrentTable(QStringLiteral("people")));

    // Previewing a file is not a licence to modify it, and a database another
    // process has open is exactly where that goes wrong.
    QSqlDatabase connection = QSqlDatabase::database(
        QSqlDatabase::connectionNames().filter(QStringLiteral("mole-sqlite")).value(0), false);
    QVERIFY(connection.isValid());

    QSqlQuery attempt(connection);
    QVERIFY2(
        !attempt.exec(QStringLiteral("DELETE FROM people")), "the preview connection must refuse to write");

    // And the data is still there.
    QCOMPARE(table.rowCountOf(QStringLiteral("people")), 4);
}

// --------------------------------------------------------------- Parquet

void TestTableSources::readsAParquetFile()
{
#ifndef MOLE_HAVE_PARQUET
    QSKIP("this build has no Parquet support");
#else
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("data.parquet"));

    arrow::Int64Builder ids;
    arrow::StringBuilder names;
    for (int i = 0; i < 5000; ++i) {
        QVERIFY(ids.Append(i).ok());
        QVERIFY(names.Append(QStringLiteral("name %1").arg(i).toStdString()).ok());
    }
    std::shared_ptr<arrow::Array> idArray;
    std::shared_ptr<arrow::Array> nameArray;
    QVERIFY(ids.Finish(&idArray).ok());
    QVERIFY(names.Finish(&nameArray).ok());

    auto schema = arrow::schema({ arrow::field("id", arrow::int64()), arrow::field("name", arrow::utf8()) });
    auto arrowTable = arrow::Table::Make(schema, { idArray, nameArray });

    auto out = arrow::io::FileOutputStream::Open(path.toStdString());
    QVERIFY(out.ok());
    // Small row groups on purpose, so the windowed read has several to choose
    // between rather than one covering everything.
    QVERIFY(
        parquet::arrow::WriteTable(*arrowTable, arrow::default_memory_pool(), out.ValueUnsafe(), 1000).ok());
    QVERIFY(out.ValueUnsafe()->Close().ok());

    ParquetTable table(path);
    QString error;
    QVERIFY2(table.open(&error), qPrintable(error));

    QCOMPARE(table.headers(), QStringList({ "id", "name" }));
    QCOMPARE(table.totalRows(), 5000);
    QVERIFY(table.columnTypes().contains(QStringLiteral("int64")));
    QVERIFY(table.fileSummary().contains(QStringLiteral("row groups")));
#endif
}

void TestTableSources::pagesThroughAParquetFileWithoutReadingItAll()
{
#ifndef MOLE_HAVE_PARQUET
    QSKIP("this build has no Parquet support");
#else
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("data.parquet"));

    arrow::Int64Builder ids;
    for (int i = 0; i < 5000; ++i)
        QVERIFY(ids.Append(i).ok());
    std::shared_ptr<arrow::Array> idArray;
    QVERIFY(ids.Finish(&idArray).ok());

    auto schema = arrow::schema({ arrow::field("id", arrow::int64()) });
    auto arrowTable = arrow::Table::Make(schema, { idArray });
    auto out = arrow::io::FileOutputStream::Open(path.toStdString());
    QVERIFY(out.ok());
    QVERIFY(
        parquet::arrow::WriteTable(*arrowTable, arrow::default_memory_pool(), out.ValueUnsafe(), 500).ok());
    QVERIFY(out.ValueUnsafe()->Close().ok());

    ParquetTable table(path);
    QVERIFY(table.open());

    // A window from the middle. Only the groups it touches are decoded, which is
    // what lets a file far larger than memory open at all.
    const QList<QStringList> window = table.rows(2500, 3);
    QCOMPARE(window.size(), 3);
    QCOMPARE(window.first().first(), QStringLiteral("2500"));
    QCOMPARE(window.last().first(), QStringLiteral("2502"));

    // The very end, which is where an off-by-one in the group arithmetic shows.
    const QList<QStringList> last = table.rows(4999, 5);
    QCOMPARE(last.size(), 1);
    QCOMPARE(last.first().first(), QStringLiteral("4999"));

    QVERIFY(table.rows(6000, 5).isEmpty());
    QCOMPARE(table.matchingRows(QStringLiteral("4999")), 1);
#endif
}

void TestTableSources::parquetReportsItselfWhenUnsupported()
{
    // Whatever this build has, the answer has to be honest: a viewer that cannot
    // read the format must say so rather than opening an empty grid.
#ifdef MOLE_HAVE_PARQUET
    QVERIFY(ParquetTable::isSupported());
#else
    QVERIFY(!ParquetTable::isSupported());
    ParquetTable table(QStringLiteral("/nonexistent.parquet"));
    QString error;
    QVERIFY(!table.open(&error));
    QVERIFY(error.contains(QStringLiteral("without Parquet")));
#endif
}

MOLE_TEST_MAIN(TestTableSources)
#include "tst_TableSources.moc"
