// Arrow first, deliberately, and before anything from Qt. Arrow declares a
// parameter called `signals`, and Qt's `signals` macro expands to `public:` --
// so with the Qt headers in front, Arrow's own headers refuse to compile.
#ifdef MOLE_HAVE_PARQUET
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/record_batch.h>
#include <arrow/util/compression.h>
#include <arrow/util/config.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>
#endif

#include "core/data/ParquetTable.h"

#include <QStringList>

#include <algorithm>
#include <functional>

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
    /// Rows Arrow has decoded for this file since it was opened. What a test
    /// asserts a bound on, because the fault this bounds is invisible in an
    /// answer that is correct either way -- see rowsDecoded().
    mutable qint64 rowsDecoded = 0;

    /// Rows handed to Arrow's reader as one batch.
    ///
    /// The unit of both memory and work: a batch is decoded whole, and nothing
    /// past the one that answers the question is decoded at all. A window is 500
    /// rows and a width sample is 200, so a few thousand answers either without
    /// touching the rest of a row group -- which is what a group written as the
    /// whole file makes the difference between reading megabytes and gigabytes.
    static constexpr int64_t kBatchRows = 4096;

    /// The most rows any filtered walk will look at.
    ///
    /// Parquet has no query engine behind it, so a filter means scanning, and a
    /// filter is an exploratory tool here rather than an analytical one. Shared
    /// by the count and the read on purpose: bounded differently, the footer
    /// would report a number of matches the grid could not page through.
    static constexpr qint64 kFilterScanLimit = 200000;

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
        // Asked once. Written as GetScalar(index).ValueOr(nullptr) ? ... it was
        // asked twice per cell, and GetScalar allocates a Scalar -- so every
        // page of the grid built two of everything it showed and threw one away.
        // See MOLE-405.
        const arrow::Result<std::shared_ptr<arrow::Scalar>> scalar = array->GetScalar(index);
        if (!scalar.ok() || !scalar.ValueUnsafe())
            return {};
        return QString::fromStdString(scalar.ValueUnsafe()->ToString());
    }

    /// Walks rows from `firstRow`, handing each one's cells to `visit`, and stops
    /// when it says so or when `maxRows` have been walked. False when a group
    /// could not be read -- which is a window that could not be read, not one
    /// that held nothing; see ITableSource::rows().
    ///
    /// **A batch at a time, and this is the whole of MOLE-287.** The obvious
    /// reading -- ReadRowGroup() and then CombineChunks() -- materialises a whole
    /// row group and copies it, and a row group is whatever the writer chose: the
    /// convention is 128 MB or a million rows, and plenty of tools write one group
    /// for the whole file. So showing fifty rows of a file like that read and
    /// decompressed all of it, into memory, and then copied it. A batch reader
    /// decodes as it is asked, so what is held is one batch and what is decoded
    /// stops where the answer does.
    bool walk(qint64 firstRow, qint64 maxRows, const std::function<bool(const QStringList&)>& visit) const
    {
        if (!reader || maxRows <= 0)
            return true;

        // Which group holds the first row wanted. The groups before it are not
        // opened at all.
        int firstGroup = 0;
        for (int group = 0; group < groupFirstRow.size(); ++group) {
            if (groupFirstRow.at(group) <= firstRow)
                firstGroup = group;
            else
                break;
        }

        qint64 skip = groupFirstRow.isEmpty() ? 0 : firstRow - groupFirstRow.at(firstGroup);
        qint64 walked = 0;

        for (int group = firstGroup; group < groupFirstRow.size() && walked < maxRows; ++group) {
            // Two spellings of one call, because the Arrow a distribution ships is
            // not the Arrow this is developed against. The Result-returning
            // overload arrived in 21.0.0 and the out-parameter one it replaced is
            // deprecated there but present; before 21 only the second exists.
            // Fedora 40 ships 15.0.2, and MOLE-287 was written against 25.0.1 --
            // so the Parquet grid did not compile at all on the family the .rpm is
            // for, which is how this was found. See MOLE-121.
            std::unique_ptr<arrow::RecordBatchReader> batches;
#if ARROW_VERSION_MAJOR >= 21
            auto opened = reader->GetRecordBatchReader(std::vector<int> { group });
            if (!opened.ok())
                return false;
            batches = std::move(opened).ValueUnsafe();
#else
            if (!reader->GetRecordBatchReader(std::vector<int> { group }, &batches).ok())
                return false;
#endif

            while (walked < maxRows) {
                std::shared_ptr<arrow::RecordBatch> batch;
                if (!batches->ReadNext(&batch).ok())
                    return false;
                if (!batch)
                    break; // the group is read out
                rowsDecoded += batch->num_rows();

                // A batch that is entirely before the window costs its decode and
                // nothing else: not one cell of it is turned into text.
                if (skip >= batch->num_rows()) {
                    skip -= batch->num_rows();
                    continue;
                }

                for (int64_t row = skip; row < batch->num_rows() && walked < maxRows; ++row) {
                    QStringList values;
                    values.reserve(batch->num_columns());
                    for (int column = 0; column < batch->num_columns(); ++column)
                        values.append(cellText(batch->column(column), row));
                    ++walked;
                    if (!visit(values))
                        return true;
                }
                skip = 0;
            }
            skip = 0;
        }
        return true;
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

    // Built rather than opened straight, because the three properties below are
    // what make a windowed read a windowed read.
    parquet::ArrowReaderProperties properties;
    // One batch is the unit of memory and of work. See Private::kBatchRows.
    properties.set_batch_size(Private::kBatchRows);
    // Off, and it matters more than the batch size: pre-buffering fetches a whole
    // row group's column chunks up front, which for a file written as one group
    // is the entire file -- exactly the read this is here to avoid.
    properties.set_pre_buffer(false);
    // One thread, this one. The source is asked one question at a time, on a task
    // (see ITableSource::canBeReadOnATask()), and a pool Arrow started underneath
    // would put the cost of an answer somewhere nobody is accounting for it.
    properties.set_use_threads(false);

    parquet::arrow::FileReaderBuilder builder;
    const arrow::Status began = builder.Open(opened.ValueUnsafe());
    if (!began.ok())
        return fail(QString::fromStdString(began.ToString()));
    builder.properties(properties);
    auto built = builder.Build();
    if (!built.ok())
        return fail(QString::fromStdString(built.status().ToString()));
    d->reader = std::move(built).ValueUnsafe();

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
    d->rowsDecoded = 0;
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

    // One walk, bounded by kFilterScanLimit -- see there for why a filter is
    // bounded at all. It used to be a loop of rows() calls, each of which found
    // its own way back to the group it wanted: with a group covering the whole
    // file, that was the file decoded again for every four thousand rows counted.
    qint64 matches = 0;
    d->walk(0, std::min(d->rowCount, Private::kFilterScanLimit), [&](const QStringList& row) {
        for (const QString& cell : row) {
            if (cell.contains(filter, Qt::CaseInsensitive)) {
                ++matches;
                break;
            }
        }
        return true;
    });
    return matches;
}

QList<QStringList> ParquetTable::rows(qint64 offset, int limit, const QString& filter, bool* readable) const
{
    if (readable)
        *readable = true;

    QList<QStringList> out;
    if (!d->reader || !d->schema || limit <= 0 || offset >= d->rowCount)
        return out;

    // Unfiltered, the window says where to start and how much to walk. Filtered,
    // there is nowhere to start but the beginning -- a format with no index owes
    // that honestly -- and the walk is bounded by the same figure the count uses,
    // so the footer never reports matches the grid cannot page through.
    const bool filtered = !filter.isEmpty();
    qint64 matched = 0;
    const bool readAll = d->walk(filtered ? 0 : offset,
        filtered ? std::min(d->rowCount, Private::kFilterScanLimit) : limit, [&](const QStringList& row) {
            if (filtered) {
                bool hit = false;
                for (const QString& cell : row) {
                    if (cell.contains(filter, Qt::CaseInsensitive)) {
                        hit = true;
                        break;
                    }
                }
                if (!hit)
                    return true;
                if (matched++ < offset)
                    return true;
            }
            out.append(row);
            return out.size() < limit;
        });

    if (!readAll && readable)
        *readable = false;
    return out;
}

qint64 ParquetTable::rowsDecoded() const
{
    return d->rowsDecoded;
}

QList<int> ParquetTable::columnWidths(int sampleRows) const
{
    QList<int> widths;
    const QStringList names = headers();
    for (const QString& name : names)
        widths.append(static_cast<int>(name.size()));

    // A sample, and nothing beyond it is decoded: the widths are a hint about how
    // to draw columns, not a claim about the file.
    d->walk(0, sampleRows, [&widths](const QStringList& row) {
        for (int i = 0; i < widths.size() && i < row.size(); ++i)
            widths[i] = std::max(widths.at(i), static_cast<int>(row.at(i).size()));
        return true;
    });
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
qint64 ParquetTable::rowsDecoded() const
{
    return 0;
}

#endif // MOLE_HAVE_PARQUET

} // namespace mole
