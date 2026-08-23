#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/TableModel.h"

#include "core/tasks/TaskManager.h"
#include "core/text/DelimitedStore.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// A table whose next window read can be told to fail.
///
/// A read the model makes while an import writes to the same file can fail on
/// its own account -- a locked database, an I/O error -- and what the model does
/// with that answer is the subject here, so the failure is arranged rather than
/// raced for. See MOLE-289 and ADR-0030.
class FlakyTable final : public ITableSource
{
public:
    explicit FlakyTable(qint64 rows)
        : m_rows(rows)
    {
    }

    void failNextRead() { m_failNext = true; }
    int reads() const { return m_reads; }

    QStringList headers() const override { return { QStringLiteral("id") }; }
    qint64 totalRows() const override { return m_rows; }
    qint64 matchingRows(const QString&) const override { return m_rows; }

    QList<QStringList> rows(
        qint64 offset, int limit, const QString& = {}, bool* readable = nullptr) const override
    {
        ++m_reads;
        if (m_failNext) {
            m_failNext = false;
            if (readable)
                *readable = false;
            return {};
        }
        if (readable)
            *readable = true;

        QList<QStringList> out;
        for (qint64 row = offset; row < qMin<qint64>(offset + limit, m_rows); ++row)
            out.append(QStringList { QString::number(row) });
        return out;
    }

    QList<int> columnWidths(int) const override { return { 8 }; }

private:
    qint64 m_rows = 0;
    mutable bool m_failNext = false;
    mutable int m_reads = 0;
};

/// A table that says it may be read on a task, and remembers where it was read.
///
/// The claim under test is about a thread, not about a duration: a Parquet file
/// written as one row group used to be read whole, on the thread that draws, to
/// show fifty rows of it. A fake rather than a real file because what is being
/// tested is the model -- whether it asks somewhere else and fills in when the
/// answer lands. See MOLE-287.
class OffThreadTable final : public ITableSource
{
public:
    OffThreadTable(qint64 rows, QThread* drawing)
        : m_rows(rows)
        , m_drawing(drawing)
    {
    }

    bool canBeReadOnATask() const override { return true; }
    QStringList headers() const override { return { QStringLiteral("id") }; }

    /// Both of these are the promise canBeReadOnATask() makes: answered from
    /// memory, so the model may go on asking them where it always did.
    qint64 totalRows() const override { return m_rows; }
    qint64 matchingRows(const QString& filter) const override
    {
        if (filter.isEmpty())
            return m_rows;
        note();
        ++m_counts;
        // Every tenth row "matches", which is a table of a different size.
        return m_rows / 10;
    }

    QList<QStringList> rows(
        qint64 offset, int limit, const QString& filter = {}, bool* readable = nullptr) const override
    {
        note();
        ++m_reads;
        if (readable)
            *readable = true;

        const qint64 stride = filter.isEmpty() ? 1 : 10;
        QList<QStringList> out;
        for (int i = 0; i < limit; ++i) {
            const qint64 row = (offset + i) * stride;
            if (row >= m_rows)
                break;
            out.append(QStringList { QString::number(row) });
        }
        return out;
    }

    QList<int> columnWidths(int sampleRows) const override
    {
        note();
        ++m_samples;
        m_sampled = sampleRows;
        return { 11 };
    }

    int reads() const { return m_reads; }
    int counts() const { return m_counts; }
    int samples() const { return m_samples; }
    int sampledRows() const { return m_sampled; }
    /// The whole point: never true.
    bool wasReadOnTheDrawingThread() const { return m_onDrawingThread; }

private:
    void note() const
    {
        if (QThread::currentThread() == m_drawing)
            m_onDrawingThread = true;
    }

    qint64 m_rows = 0;
    QThread* m_drawing = nullptr;
    mutable std::atomic<int> m_reads { 0 };
    mutable std::atomic<int> m_counts { 0 };
    mutable std::atomic<int> m_samples { 0 };
    mutable std::atomic<int> m_sampled { 0 };
    mutable std::atomic<bool> m_onDrawingThread { false };
};

} // namespace

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
    void aWindowThatCouldNotBeReadIsNotKeptAsAnEmptyOne();
    void aSourceThatSaysSoIsReadOnATaskAndNotFromData();
    void aFilteredCountIsTakenOnATaskAndTheGridWaitsForIt();
    void steppingToAnotherPageDropsWhatTheOldOneAskedFor();

private:
    /// A store of `rows` rows, which the model reads through like any other
    /// ITableSource. A delimited import rather than a database because it is
    /// the cheapest source to fill: what is being tested is the model.
    std::shared_ptr<DelimitedStore> storeOf(int rows);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<DelimitedStore> m_store;
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

std::shared_ptr<DelimitedStore> TestTableModel::storeOf(int rows)
{
    auto store
        = std::make_shared<DelimitedStore>(QDir(m_dir->path()).filePath(QStringLiteral("rows.sqlite")));
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
    model.setSource(m_store);

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
    model.setSource(m_store);

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
    model.setSource(m_store);
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
    model.setSource(m_store);

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
    model.setSource(m_store);
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
    model.setSource(m_store);
    QCOMPARE(model.page(), 0);
}

void TestTableModel::aBlockAskedForPastTheEndOfThePageStopsAtIt()
{
    m_store = storeOf(12000);
    QVERIFY(m_store);

    TableModel model;
    model.setSource(m_store);

    // A selection cannot span pages, because a row index means a row on the
    // page. Asked for one that runs past the end, the block stops there rather
    // than reaching into rows the view was never offered.
    const QString block = model.blockAsText(4998, 0, 6000, 0);
    const QStringList lines = block.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.first(), QStringLiteral("4998"));
    QCOMPARE(lines.last(), QStringLiteral("4999"));
}

void TestTableModel::aWindowThatCouldNotBeReadIsNotKeptAsAnEmptyOne()
{
    auto source = std::make_shared<FlakyTable>(2000);
    TableModel model;
    model.setSource(source);
    QCOMPARE(model.rowCount(), 2000);

    // The read behind the first cell fails. A blank cell is the right answer to
    // that: the model has nothing to show and does not invent a nothing.
    source->failNextRead();
    QVERIFY(!model.data(model.index(0, 0), TableModel::CellRole).isValid());

    // What must not happen is that it is remembered. Nothing since has cleared
    // the cache -- no refresh, no page move, no filter -- so an empty window
    // kept from a failed read would leave this block of the grid blank until one
    // of those came along, which during an import is the next batch and may be
    // seconds off.
    QCOMPARE(model.data(model.index(0, 0), TableModel::CellRole).toString(), QStringLiteral("0"));
    QCOMPARE(model.data(model.index(499, 0), TableModel::CellRole).toString(), QStringLiteral("499"));

    // And the retry is one read, not one per cell: a window that was read is
    // still cached the way it always was.
    const int reads = source->reads();
    QCOMPARE(model.data(model.index(1, 0), TableModel::CellRole).toString(), QStringLiteral("1"));
    QCOMPARE(source->reads(), reads);
}

void TestTableModel::aSourceThatSaysSoIsReadOnATaskAndNotFromData()
{
    TaskManager tasks;
    auto source = std::make_shared<OffThreadTable>(12000, QThread::currentThread());

    TableModel model;
    model.setSource(source, &tasks);

    // The shape of the table is known at once -- headers and the unfiltered count
    // are what the source promised to answer from memory -- and nothing has been
    // read yet.
    QCOMPARE(model.headers(), QStringList { QStringLiteral("id") });
    QCOMPARE(model.totalRows(), 12000);
    QCOMPARE(model.rowCount(), TableModel::kPageRows);
    QCOMPARE(source->reads(), 0);

    // Asking for a cell does not read it. This is the fault: data() used to call
    // straight into the source, so a window of fifty rows out of a Parquet file
    // written as one row group was the whole file, on this thread.
    QVERIFY(!model.data(model.index(0, 0), TableModel::CellRole).isValid());
    QCOMPARE(source->reads(), 0);
    QVERIFY2(model.isReading(), "the ask has to be outstanding, or nothing was asked");

    // And it arrives. Waited for on the model saying it is done rather than on a
    // clock: how long a pool thread takes is the machine's business.
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));
    QCOMPARE(model.data(model.index(0, 0), TableModel::CellRole).toString(), QStringLiteral("0"));
    QCOMPARE(model.data(model.index(499, 0), TableModel::CellRole).toString(), QStringLiteral("499"));
    QVERIFY2(!source->wasReadOnTheDrawingThread(), "the thread that draws read the file");

    // The widths are a read too, and they were asked for the same way -- a sample,
    // not the file.
    QCOMPARE(source->samples(), 1);
    QCOMPARE(source->sampledRows(), 200);
    QCOMPARE(model.columnWidths(), QVariantList { 11 });

    // A chunk already fetched is not fetched again, exactly as before.
    const int reads = source->reads();
    QCOMPARE(model.data(model.index(1, 0), TableModel::CellRole).toString(), QStringLiteral("1"));
    QCOMPARE(source->reads(), reads);

    // One question at a time: a screenful of cells scattered over the page asks
    // for several chunks and they are read one after another, because a source is
    // not promised to be usable from two threads at once.
    for (int row = 600; row < 4000; row += 600)
        model.data(model.index(row, 0), TableModel::CellRole);
    QVERIFY(model.isReading());
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));
    QCOMPARE(model.data(model.index(3600, 0), TableModel::CellRole).toString(), QStringLiteral("3600"));
    QVERIFY(!source->wasReadOnTheDrawingThread());
}

void TestTableModel::aFilteredCountIsTakenOnATaskAndTheGridWaitsForIt()
{
    TaskManager tasks;
    auto source = std::make_shared<OffThreadTable>(12000, QThread::currentThread());

    TableModel model;
    model.setSource(source, &tasks);
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));

    // A filter is a scan -- no index answers a substring match across every
    // column -- and it is the other place the thread that draws used to wait.
    model.setFilter(QStringLiteral("7"));
    model.applyFilter();

    // Until the count lands the table has no size, and -1 is how this model has
    // always said so: the footer shows a blank rather than a nought that would
    // read as a table with nothing in it.
    QCOMPARE(model.matchingRows(), -1);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(source->counts(), 0);
    QVERIFY(model.isReading());

    QVERIFY(waitFor([&model] { return model.matchingRows() >= 0; }, 10000));
    QCOMPARE(model.matchingRows(), 1200);
    QCOMPARE(model.rowCount(), 1200);
    QCOMPARE(model.page(), 0);

    // And the rows of the filtered table arrive the same way.
    QVERIFY(!model.data(model.index(0, 0), TableModel::CellRole).isValid());
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));
    QCOMPARE(model.data(model.index(0, 0), TableModel::CellRole).toString(), QStringLiteral("0"));
    QCOMPARE(model.data(model.index(1, 0), TableModel::CellRole).toString(), QStringLiteral("10"));
    QVERIFY2(!source->wasReadOnTheDrawingThread(), "the thread that draws counted the matches");
}

void TestTableModel::steppingToAnotherPageDropsWhatTheOldOneAskedFor()
{
    TaskManager tasks;
    auto source = std::make_shared<OffThreadTable>(12000, QThread::currentThread());

    TableModel model;
    model.setSource(source, &tasks);
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));

    // Several chunks of this page asked for, so there is a queue behind the one
    // being read.
    for (int row = 0; row < 4000; row += 500)
        model.data(model.index(row, 0), TableModel::CellRole);
    QVERIFY(model.isReading());

    // The reader turns the page. Everything outstanding was asked for at an offset
    // inside the page being left, so it goes with it -- otherwise a stripe of the
    // new page fills in with rows from the old one.
    model.nextPage();
    QVERIFY2(!model.isReading(), "the outgoing page's reads were kept");
    QCOMPARE(model.firstRowOnPage(), TableModel::kPageRows);

    // The new page reads what it needs, and the rows are its own.
    QVERIFY(!model.data(model.index(0, 0), TableModel::CellRole).isValid());
    QVERIFY(waitFor([&model] { return !model.isReading(); }, 10000));
    QCOMPARE(model.data(model.index(0, 0), TableModel::CellRole).toString(),
        QString::number(TableModel::kPageRows));
    QVERIFY(!source->wasReadOnTheDrawingThread());
}

MOLE_TEST_MAIN(TestTableModel)
#include "tst_TableModel.moc"
