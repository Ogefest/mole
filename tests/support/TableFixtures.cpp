// Arrow first, before anything from Qt: it declares a parameter called
// `signals`, and Qt's macro of that name expands to `public:`.
#ifdef MOLE_HAVE_PARQUET
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#endif

#include "support/TableFixtures.h"

#include <QFont>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPolygon>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace mole::test::fixtures {

bool writeSqlite(const QString& path)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return false;

    // A connection name of its own, removed afterwards: two fixtures built in one
    // process must not fight over the default connection.
    const QString name = QStringLiteral("fixture-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (!db.open())
            return false;

        QSqlQuery query(db);
        const QStringList statements {
            QStringLiteral("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER)"),
            QStringLiteral("INSERT INTO people VALUES (1,'Ada','Kraków',36)"),
            QStringLiteral("INSERT INTO people VALUES (2,'Grace','Berlin',45)"),
            // A null and an empty string, which a viewer has to tell apart.
            QStringLiteral("INSERT INTO people VALUES (3,'Alan',NULL,41)"),
            QStringLiteral("INSERT INTO people VALUES (4,'Edsger','',38)"),
            QStringLiteral("INSERT INTO people VALUES (5,'Barbara','Lisbon',52)"),
            QStringLiteral("INSERT INTO people VALUES (6,'Tony','Bristol',29)"),
            // A second table whose name is a reserved word, so quoting is exercised.
            QStringLiteral(R"(CREATE TABLE "order" (ref TEXT, placed TEXT))"),
            QStringLiteral(R"(INSERT INTO "order" VALUES ('A-1','2026-02-11'))"),
            QStringLiteral(R"(INSERT INTO "order" VALUES ('A-2','2026-03-02'))"),
            QStringLiteral("CREATE VIEW adults AS SELECT name FROM people WHERE age >= 40"),
        };
        for (const QString& statement : statements)
            ok = query.exec(statement) && ok;
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool parquetAvailable()
{
#ifdef MOLE_HAVE_PARQUET
    return true;
#else
    return false;
#endif
}

bool writeParquet(const QString& path, int rows)
{
#ifndef MOLE_HAVE_PARQUET
    Q_UNUSED(path);
    Q_UNUSED(rows);
    return false;
#else
    arrow::Int64Builder ids;
    arrow::StringBuilder names;
    arrow::DoubleBuilder amounts;
    for (int i = 0; i < rows; ++i) {
        if (!ids.Append(i).ok())
            return false;
        if (!names.Append(QStringLiteral("row %1").arg(i).toStdString()).ok())
            return false;
        if (!amounts.Append(i * 1.5).ok())
            return false;
    }

    std::shared_ptr<arrow::Array> idArray;
    std::shared_ptr<arrow::Array> nameArray;
    std::shared_ptr<arrow::Array> amountArray;
    if (!ids.Finish(&idArray).ok() || !names.Finish(&nameArray).ok() || !amounts.Finish(&amountArray).ok())
        return false;

    auto schema = arrow::schema({ arrow::field("id", arrow::int64()), arrow::field("name", arrow::utf8()),
        arrow::field("amount", arrow::float64()) });
    auto table = arrow::Table::Make(schema, { idArray, nameArray, amountArray });

    auto out = arrow::io::FileOutputStream::Open(path.toStdString());
    if (!out.ok())
        return false;
    // Small row groups on purpose, so a windowed read has several to choose
    // between rather than one covering everything.
    if (!parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out.ValueUnsafe(), 1000).ok())
        return false;
    return out.ValueUnsafe()->Close().ok();
#endif
}

bool writeLargeImage(const QString& path)
{
    // Bigger than any pane it will be shown in, so the picture of it shows the
    // viewer fitting an image to the space rather than an image that happened to
    // fit. Drawn rather than committed: see the header.
    QImage image(2400, 1600, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QLinearGradient sky(0, 0, 0, image.height());
    sky.setColorAt(0.0, QColor("#12203a"));
    sky.setColorAt(0.6, QColor("#3b6ea5"));
    sky.setColorAt(1.0, QColor("#c9d7e6"));
    painter.fillRect(image.rect(), sky);

    // Something with edges in it, so scaling is visibly doing something.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#0b1622"));
    for (int i = 0; i < 6; ++i) {
        const int base = image.height() - 240 - i * 30;
        QPolygon ridge;
        ridge << QPoint(i * 420 - 200, image.height());
        ridge << QPoint(i * 420 + 180, base);
        ridge << QPoint(i * 420 + 520, image.height());
        painter.drawPolygon(ridge);
    }

    painter.setBrush(QColor("#f3e2a9"));
    painter.drawEllipse(QPoint(1900, 340), 120, 120);

    painter.setPen(QColor("#e8eef7"));
    QFont label = painter.font();
    label.setPixelSize(64);
    painter.setFont(label);
    painter.drawText(QRect(80, 80, 1200, 120), Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("2400 x 1600, drawn by the test suite"));

    painter.end();
    return image.save(path, "PNG");
}

} // namespace mole::test::fixtures
