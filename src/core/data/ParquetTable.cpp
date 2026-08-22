// Arrow first, deliberately, and before anything from Qt. Arrow declares a
// parameter called `signals`, and Qt's `signals` macro expands to `public:` --
// so with the Qt headers in front, Arrow's own headers refuse to compile.
#ifdef MOLE_HAVE_PARQUET
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#endif

#include "core/data/ParquetTable.h"

#include <QStringList>

#include <algorithm>

namespace mole {

#ifdef MOLE_HAVE_PARQUET

struct ParquetTable::Private
{
    QString path;
    std::unique_ptr<parquet::arrow::FileReader> reader;
    std::shared_ptr<arrow::Schema> schema;
    qint64 rowCount = 0;
    QString summary;
    /// Row group boundaries, so a window can be turned into "which groups".
    QList<qint64> groupFirstRow;

    /// One cell as text. Arrow's own formatting, which is the only sane choice:
    /// reimplementing date, decimal and nested formatting would be a second,
    /// worse Arrow.
    static QString cellText(const std::shared_ptr<arrow::Array>& array, int64_t index)
    {
        if (!array || index >= array->length())
            return {};
        // A null is not an empty string, and showing them alike hides the
        // difference the reader is usually looking for.
        if (array->IsNull(index))
            return QStringLiteral("NULL");
        return QString::fromStdString(array->GetScalar(index).ValueOr(nullptr)
                ? array->GetScalar(index).ValueUnsafe()->ToString()
                : std::string());
    }
};

ParquetTable::ParquetTable(QString path)
    : d(std::make_unique<Private>())
{
    d->path = std::move(path);
}

ParquetTable::~ParquetTable() = default;

bool ParquetTable::isSupported()
{
    return true;
}

bool ParquetTable::open(QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    auto opened = arrow::io::ReadableFile::Open(d->path.toStdString());
    if (!opened.ok())
        return fail(QString::fromStdString(opened.status().ToString()));

    auto builderResult = parquet::arrow::OpenFile(opened.ValueUnsafe(), arrow::default_memory_pool());
    if (!builderResult.ok())
        return fail(QString::fromStdString(builderResult.status().ToString()));
    d->reader = std::move(builderResult).ValueUnsafe();

    if (!d->reader->GetSchema(&d->schema).ok() || !d->schema)
        return fail(QStringLiteral("This file has no readable schema"));

    const auto* metadata = d->reader->parquet_reader()->metadata().get();
    d->rowCount = metadata->num_rows();

    qint64 first = 0;
    for (int group = 0; group < metadata->num_row_groups(); ++group) {
        d->groupFirstRow.append(first);
        first += metadata->RowGroup(group)->num_rows();
    }

    QString compression = QStringLiteral("none");
    if (metadata->num_row_groups() > 0 && metadata->RowGroup(0)->num_columns() > 0) {
        compression = QString::fromStdString(
            arrow::util::Codec::GetCodecAsString(metadata->RowGroup(0)->ColumnChunk(0)->compression()));
    }

    d->summary = QStringLiteral("%1 row groups · %2 · %3")
                     .arg(metadata->num_row_groups())
                     .arg(compression, QString::fromStdString(metadata->created_by()));
    return true;
}

void ParquetTable::close()
{
    d->reader.reset();
    d->schema.reset();
    d->groupFirstRow.clear();
    d->rowCount = 0;
}

bool ParquetTable::isOpen() const
{
    return d->reader != nullptr;
}

QString ParquetTable::fileSummary() const
{
    return d->summary;
}

QStringList ParquetTable::headers() const
{
    QStringList out;
    if (!d->schema)
        return out;
    for (int i = 0; i < d->schema->num_fields(); ++i)
        out.append(QString::fromStdString(d->schema->field(i)->name()));
    return out;
}

QStringList ParquetTable::columnTypes() const
{
    QStringList out;
    if (!d->schema)
        return out;
    for (int i = 0; i < d->schema->num_fields(); ++i)
        out.append(QString::fromStdString(d->schema->field(i)->type()->ToString()));
    return out;
}

qint64 ParquetTable::totalRows() const
{
    return d->rowCount;
}

qint64 ParquetTable::matchingRows(const QString& filter) const
{
    if (filter.isEmpty())
        return d->rowCount;

    // Parquet has no query engine behind it, so a filter means scanning. Bounded
    // deliberately: an unbounded scan of a multi-gigabyte file on the interface
    // thread would freeze the window, and a filter is an exploratory tool here
    // rather than an analytical one.
    qint64 matches = 0;
    qint64 scanned = 0;
    constexpr qint64 kScanLimit = 200000;
    constexpr int kBatch = 4096;

    while (scanned < std::min(d->rowCount, kScanLimit)) {
        const QList<QStringList> batch = rows(scanned, kBatch);
        if (batch.isEmpty())
            break;
        for (const QStringList& row : batch) {
            for (const QString& cell : row) {
                if (cell.contains(filter, Qt::CaseInsensitive)) {
                    ++matches;
                    break;
                }
            }
        }
        scanned += batch.size();
    }
    return matches;
}

QList<QStringList> ParquetTable::rows(qint64 offset, int limit, const QString& filter, bool* readable) const
{
    if (readable)
        *readable = true;

    QList<QStringList> out;
    if (!d->reader || !d->schema || limit <= 0 || offset >= d->rowCount)
        return out;

    // Unfiltered: read only the row groups the window touches. Filtered: scan
    // forward from the start, which is the honest cost of a format with no index.
    const qint64 readFrom = filter.isEmpty() ? offset : 0;
    const qint64 wanted = filter.isEmpty() ? limit : std::min<qint64>(d->rowCount, 200000);

    // Which groups cover the range.
    int firstGroup = 0;
    for (int group = 0; group < d->groupFirstRow.size(); ++group) {
        if (d->groupFirstRow.at(group) <= readFrom)
            firstGroup = group;
        else
            break;
    }

    qint64 skipped = d->groupFirstRow.isEmpty() ? 0 : readFrom - d->groupFirstRow.at(firstGroup);
    qint64 produced = 0;
    qint64 matched = 0;

    for (int group = firstGroup; group < d->groupFirstRow.size() && produced < wanted; ++group) {
        std::shared_ptr<arrow::Table> table;
        if (!d->reader->ReadRowGroup(group, &table).ok() || !table) {
            // A group Arrow refused is a window that could not be read, not one
            // that held nothing -- see ITableSource::rows().
            if (readable)
                *readable = false;
            break;
        }

        const int64_t rowsHere = table->num_rows();
        // Columns arrive as chunked arrays; combining once per group keeps the
        // indexing below simple without copying the whole file.
        auto combined = table->CombineChunks(arrow::default_memory_pool());
        if (!combined.ok()) {
            if (readable)
                *readable = false;
            break;
        }
        table = combined.ValueUnsafe();

        for (int64_t row = skipped; row < rowsHere && produced < wanted; ++row) {
            QStringList values;
            values.reserve(table->num_columns());
            for (int column = 0; column < table->num_columns(); ++column) {
                const auto chunked = table->column(column);
                values.append(
                    Private::cellText(chunked->num_chunks() > 0 ? chunked->chunk(0) : nullptr, row));
            }

            if (!filter.isEmpty()) {
                bool hit = false;
                for (const QString& cell : values) {
                    if (cell.contains(filter, Qt::CaseInsensitive)) {
                        hit = true;
                        break;
                    }
                }
                if (!hit)
                    continue;
                if (matched++ < offset)
                    continue;
            }

            out.append(values);
            ++produced;
            if (!filter.isEmpty() && out.size() >= limit)
                return out;
        }
        skipped = 0;
    }
    return out;
}

QList<int> ParquetTable::columnWidths(int sampleRows) const
{
    QList<int> widths;
    const QStringList names = headers();
    for (const QString& name : names)
        widths.append(static_cast<int>(name.size()));

    const QList<QStringList> sample = rows(0, sampleRows);
    for (const QStringList& row : sample) {
        for (int i = 0; i < widths.size() && i < row.size(); ++i)
            widths[i] = std::max(widths.at(i), static_cast<int>(row.at(i).size()));
    }
    return widths;
}

#else // MOLE_HAVE_PARQUET

// Arrow is not available in this build. The type still exists so nothing above
// has to be conditionally compiled -- it simply reports that it cannot help,
// and the preview layer falls through to the file-information viewer.
struct ParquetTable::Private
{
    QString path;
};

ParquetTable::ParquetTable(QString path)
    : d(std::make_unique<Private>())
{
    d->path = std::move(path);
}

ParquetTable::~ParquetTable() = default;

bool ParquetTable::isSupported()
{
    return false;
}

bool ParquetTable::open(QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("This build was made without Parquet support");
    return false;
}

void ParquetTable::close() { }
bool ParquetTable::isOpen() const
{
    return false;
}
QString ParquetTable::fileSummary() const
{
    return {};
}
QStringList ParquetTable::columnTypes() const
{
    return {};
}
QStringList ParquetTable::headers() const
{
    return {};
}
qint64 ParquetTable::totalRows() const
{
    return 0;
}
qint64 ParquetTable::matchingRows(const QString&) const
{
    return 0;
}
QList<QStringList> ParquetTable::rows(qint64, int, const QString&, bool* readable) const
{
    if (readable)
        *readable = true;
    return {};
}
QList<int> ParquetTable::columnWidths(int) const
{
    return {};
}

#endif // MOLE_HAVE_PARQUET

} // namespace mole
