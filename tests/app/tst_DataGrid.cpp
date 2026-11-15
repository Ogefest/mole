#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/models/TableModel.h"

#include "core/CoreMetaTypes.h"
#include "core/data/ITableSource.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

/// Three pages of a table whose first column is longer than the grid's cap, so a
/// drag is the only way to see all of it.
class WideTable final : public ITableSource
{
public:
    explicit WideTable(QStringList headers)
        : m_headers(std::move(headers))
    {
    }

    QStringList headers() const override { return m_headers; }
    qint64 totalRows() const override { return 12'000; }
    qint64 matchingRows(const QString& filter) const override { return filter.isEmpty() ? 12'000 : 3; }
    QList<QStringList> rows(qint64 offset, int limit, const QString& filter, bool* readable) const override
    {
        if (readable)
            *readable = true;
        QList<QStringList> out;
        const qint64 end = qMin<qint64>(offset + limit, matchingRows(filter));
        for (qint64 row = offset; row < end; ++row) {
            QStringList cells;
            for (int column = 0; column < m_headers.size(); ++column)
                cells
                    << QStringLiteral("/a/rather/long/path/to/somewhere/row-%1/col-%2").arg(row).arg(column);
            out << cells;
        }
        return out;
    }
    // Eighty characters in the first column, so the computed width is the cap.
    QList<int> columnWidths(int) const override
    {
        QList<int> widths { 80 };
        while (widths.size() < m_headers.size())
            widths << 6;
        return widths;
    }

private:
    QStringList m_headers;
};

} // namespace

/// Dragging a column of a table preview wider or narrower. All four tabular
/// viewers share DataGrid, and its header row is a separate Repeater rather than
/// a HorizontalHeaderView, so what is held here is that the header and the cells
/// agree to the pixel after every kind of change. See MOLE-453.
class TestDataGrid : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aDraggedColumnIsThatWideInTheHeaderAndTheCells();
    void aDoubleClickPutsTheComputedWidthBack();
    void aPageOrAFilterKeepsTheWidthAndAnotherTableDropsIt();

private:
    QQuickItem* header(int column) const;
    QQuickItem* handle(int column) const;
    qreal cellWidth(int column) const;
    /// Drags the edge of `column` by `dx` pixels, in steps, the way a hand does.
    void dragEdge(int column, int dx);

    std::unique_ptr<QmlAppHarness> m_harness;
    std::unique_ptr<TableModel> m_table;
    std::unique_ptr<QQuickItem> m_grid;
    QQuickItem* m_view = nullptr;
};

void TestDataGrid::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));

    m_table = std::make_unique<TableModel>();
    m_table->setSource(std::make_shared<WideTable>(
        QStringList { QStringLiteral("path"), QStringLiteral("size"), QStringLiteral("kind") }));

    QQmlEngine* engine = qmlEngine(m_harness->window());
    QVERIFY(engine);
    QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/DataGrid.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    m_grid.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(
        { { QStringLiteral("table"), QVariant::fromValue(static_cast<QObject*>(m_table.get())) } })));
    QVERIFY2(m_grid, qPrintable(component.errorString()));

    // Over everything else in the window, so the pointer reaches it.
    m_grid->setParentItem(m_harness->window()->contentItem());
    m_grid->setZ(1000);
    m_grid->setWidth(900);
    m_grid->setHeight(500);

    m_view = QmlAppHarness::itemIn(m_grid.get(), QStringLiteral("tableGrid"));
    QVERIFY(m_view);
    QVERIFY(m_harness->until([this] { return header(0) && header(0)->width() > 0 && cellWidth(0) > 0; }));
}

void TestDataGrid::cleanup()
{
    m_grid.reset();
    m_table.reset();
    m_harness.reset();
}

QQuickItem* TestDataGrid::header(int column) const
{
    return QmlAppHarness::itemIn(m_grid.get(), QStringLiteral("gridHeader%1").arg(column));
}

QQuickItem* TestDataGrid::handle(int column) const
{
    return QmlAppHarness::itemIn(m_grid.get(), QStringLiteral("gridResize%1").arg(column));
}

qreal TestDataGrid::cellWidth(int column) const
{
    qreal width = -1;
    QMetaObject::invokeMethod(m_view, "columnWidth", Q_RETURN_ARG(qreal, width), Q_ARG(int, column));
    return width;
}

void TestDataGrid::dragEdge(int column, int dx)
{
    QQuickItem* edge = handle(column);
    QVERIFY(edge);
    const QPoint from = m_harness->centreOf(edge);
    m_harness->press(from);
    const int steps = 10;
    for (int step = 1; step <= steps; ++step)
        m_harness->moveTo(from + QPoint(dx * step / steps, 0));
    m_harness->release(from + QPoint(dx, 0));
    m_harness->settle();
}

void TestDataGrid::aDraggedColumnIsThatWideInTheHeaderAndTheCells()
{
    const qreal computed = header(0)->width();
    QCOMPARE(cellWidth(0), computed);

    // Past the sixty-character cap, which is the reason to drag at all.
    dragEdge(0, 300);
    QVERIFY(m_harness->until([&] { return qAbs(header(0)->width() - (computed + 300)) <= 1; }));
    QCOMPARE(cellWidth(0), header(0)->width());

    // The cursor stayed where it was: a drag in the header is not a selection.
    QCOMPARE(m_grid->property("cursorRow").toInt(), -1);
    QCOMPARE(m_grid->property("anchorRow").toInt(), -1);

    // Scrolled sideways, the second column's header still starts where its cells
    // do -- to the pixel.
    m_view->setProperty("contentX", 120);
    m_harness->settle();
    const qreal headerAt = header(1)->mapToScene(QPointF(0, 0)).x();
    const qreal cellsAt = m_view->mapToScene(QPointF(0, 0)).x() - 120 + cellWidth(0);
    QVERIFY2(qAbs(headerAt - cellsAt) < 0.5,
        qPrintable(QStringLiteral("header at %1, cells at %2").arg(headerAt).arg(cellsAt)));
    m_view->setProperty("contentX", 0);
    m_harness->settle();

    // And narrower, down to a floor and no further.
    dragEdge(0, -2000);
    QVERIFY(m_harness->until([&] { return header(0)->width() < computed; }));
    QVERIFY(header(0)->width() > 0);
    QCOMPARE(header(0)->width(), qreal(m_grid->property("minimumColumnPixels").toInt()));
    QCOMPARE(cellWidth(0), header(0)->width());
}

void TestDataGrid::aDoubleClickPutsTheComputedWidthBack()
{
    const qreal computed = header(0)->width();
    dragEdge(0, 200);
    QVERIFY(m_harness->until([&] { return header(0)->width() > computed + 150; }));

    m_harness->doubleClick(m_harness->centreOf(handle(0)));
    QVERIFY(m_harness->until([&] { return header(0)->width() == computed; }));
    QCOMPARE(cellWidth(0), computed);
}

void TestDataGrid::aPageOrAFilterKeepsTheWidthAndAnotherTableDropsIt()
{
    const qreal computed = header(0)->width();
    dragEdge(0, 250);
    QVERIFY(m_harness->until([&] { return header(0)->width() > computed + 200; }));
    const qreal dragged = header(0)->width();

    // A new page is a model reset, and it keeps the width.
    QCOMPARE(m_table->pageCount(), 3);
    m_table->nextPage();
    QVERIFY(m_harness->until([this] { return m_table->page() == 1; }));
    m_harness->settle();
    QCOMPARE(header(0)->width(), dragged);
    QCOMPARE(cellWidth(0), dragged);

    // So does a filter.
    m_table->setFilter(QStringLiteral("row-1"));
    QVERIFY(m_harness->until([this] { return m_table->matchingRows() == 3; }));
    m_harness->settle();
    QCOMPARE(header(0)->width(), dragged);
    QCOMPARE(cellWidth(0), dragged);

    // Another table -- another file, or another table of the same database --
    // starts from what its own contents give.
    m_table->setSource(
        std::make_shared<WideTable>(QStringList { QStringLiteral("name"), QStringLiteral("rowid") }));
    QVERIFY(m_harness->until([&] { return header(0) && header(0)->width() == computed; }));
    QCOMPARE(cellWidth(0), computed);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestDataGrid testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_DataGrid.moc"
