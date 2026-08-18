#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/TableModel.h"

#include "core/text/DelimitedStore.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The page the grid shows.
///
/// A table used to be offered whole: rowCount() returned the matching count, so
/// ten million rows were ten million rows behind one scrollbar and the offset
/// the model fetched at was bounded by nothing. See ADR-0045.
class TestTableModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPageIsNeverMoreThanFiveThousandRows();
    void theLastPageHoldsWhatIsLeft();
    void aPageStartsWhereTheOneBeforeItEnded();
    void movingPastEitherEndChangesNothing();
    void aFilterPutsTheViewBackOnTheFirstPage();
    void aBlockAskedForPastTheEndOfThePageStopsAtIt();

private:
    /// A store of `rows` rows, which the model reads through like any other
    /// ITableSource. A delimited import rather than a database because it is
    /// the cheapest source to fill: what is being tested is the model.
    std::unique_ptr<DelimitedStore> storeOf(int rows);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DelimitedStore> m_store;
};

void TestTableModel::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestTableModel::cleanup()
{
    m_store.reset();
    m_dir.reset();
}

std::unique_ptr<DelimitedStore> TestTableModel::storeOf(int rows)
{
    auto store
        = std::make_unique<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("rows.sqlite")));
    if (!store->open() || !store->beginImport({ QStringLiteral("id"), QStringLiteral("name") }))
        return {};

    QList<QStringList> batch;
    batch.reserve(rows);
    for (int i = 0; i < rows; ++i)
        batch.append({ QString::number(i), QStringLiteral("name %1").arg(i) });
    if (!store->addRows(batch) || !store->endImport())
        return {};
    return store;
}

void TestTableModel::aPageIsNeverMoreThanFiveThousandRows()
{
    // Twelve thousand rows: two full pages and a short one, so there is a last
    // page that is not the same shape as the others.
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());

    QCOMPARE(model.totalRows(), 12000);
    QCOMPARE(model.pageCount(), 3);
    QCOMPARE(model.page(), 0);
    QCOMPARE(model.rowCount(), TableModel::kPageRows);
    QCOMPARE(model.firstRowOnPage(), 0);

    // The claim, on every page there is: the view is never offered more than a
    // page, so the largest offset any query can carry is bounded.
    for (int page = 0; page < model.pageCount(); ++page) {
        model.nextPage();
        QVERIFY2(model.rowCount() <= TableModel::kPageRows,
            qPrintable(QStringLiteral("page %1 offered %2 rows").arg(page).arg(model.rowCount())));
    }
}

void TestTableModel::theLastPageHoldsWhatIsLeft()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());

    model.lastPage();
    QCOMPARE(model.page(), 2);
    QCOMPARE(model.rowCount(), 2000);
    QCOMPARE(model.firstRowOnPage(), 10000);
    QCOMPARE(model.cellAt(1999, 0), QStringLiteral("11999"));
    // And nothing past it: the page ends where the table does.
    QCOMPARE(model.cellAt(2000, 0), QString());
}

void TestTableModel::aPageStartsWhereTheOneBeforeItEnded()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());
    QCOMPARE(model.cellAt(0, 0), QStringLiteral("0"));

    QSignalSpy paged(&model, &TableModel::pageChanged);
    model.nextPage();
    QCOMPARE(paged.count(), 1);
    QCOMPARE(model.page(), 1);

    // Row nought of the second page is row five thousand of the table. The
    // fetch window lives inside the page, so this is the only place the two
    // coordinate systems meet -- and it is the one that used to be an offset
    // into the whole file.
    QCOMPARE(model.firstRowOnPage(), TableModel::kPageRows);
    QCOMPARE(model.cellAt(0, 0), QStringLiteral("5000"));
    QCOMPARE(model.cellAt(0, 1), QStringLiteral("name 5000"));

    model.previousPage();
    QCOMPARE(model.page(), 0);
    QCOMPARE(model.cellAt(0, 0), QStringLiteral("0"));
}

void TestTableModel::movingPastEitherEndChangesNothing()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());

    QSignalSpy paged(&model, &TableModel::pageChanged);
    model.previousPage();
    QCOMPARE(model.page(), 0);
    QCOMPARE(paged.count(), 0);

    model.lastPage();
    QCOMPARE(model.page(), 2);
    model.nextPage();
    QCOMPARE(model.page(), 2);
    // Two moves that went somewhere, and two that had nowhere to go. The ends
    // of a table are where a reader expects to stop, not to wrap.
    QCOMPARE(paged.count(), 1);
}

void TestTableModel::aFilterPutsTheViewBackOnTheFirstPage()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());
    model.lastPage();
    QCOMPARE(model.page(), 2);

    // Page three of the old set of rows means nothing in the new one.
    model.setFilter(QStringLiteral("name 1"));
    model.applyFilter();
    QCOMPARE(model.page(), 0);
    QCOMPARE(model.firstRowOnPage(), 0);
    QVERIFY(model.rowCount() > 0);
    QCOMPARE(model.cellAt(0, 1), QStringLiteral("name 1"));

    // And so does the page it was on when a source is exchanged for another.
    model.setFilter(QString());
    model.applyFilter();
    model.lastPage();
    QCOMPARE(model.page(), 2);
    model.setSource(m_store.get());
    QCOMPARE(model.page(), 0);
}

void TestTableModel::aBlockAskedForPastTheEndOfThePageStopsAtIt()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store.get());

    // A selection cannot span pages, because a row index means a row on the
    // page. Asked for one that runs past the end, the block stops there rather
    // than reaching into rows the view was never offered.
    const QString block = model.blockAsText(4998, 0, 6000, 0);
    const QStringList lines = block.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.first(), QStringLiteral("4998"));
    QCOMPARE(lines.last(), QStringLiteral("4999"));
}

MOLE_TEST_MAIN(TestTableModel)
#include "tst_TableModel.moc"
