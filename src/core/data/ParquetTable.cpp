// The application's side of the Parquet preview. Arrow is not here: the reader
// lives in `mole-parquet.so`, which ParquetModule loads at run time when it is
// installed and loadable, and this class forwards to what it makes. Without the
// module everything here answers "nothing", isSupported() is false, and the
// preview falls back to the file-information view. See ParquetReader.h and
// MOLE-459.

#include "core/data/ParquetTable.h"

#include "core/data/ParquetModule.h"

#include <QStringList>

namespace mole {

struct ParquetTable::Private
{
    QString path;
    std::unique_ptr<ParquetReader> reader;
};

ParquetTable::ParquetTable(QString path)
    : d(std::make_unique<Private>())
{
    d->path = std::move(path);
}

ParquetTable::~ParquetTable() = default;

bool ParquetTable::isSupported()
{
    return parquetModule::current().loaded;
}

QString ParquetTable::diagnosticLine()
{
    return parquetModule::diagnosticLine();
}

bool ParquetTable::open(QString* errorOut)
{
    close();
    std::unique_ptr<ParquetReader> reader = parquetModule::createReader(d->path);
    if (!reader) {
        if (errorOut) {
            const parquetModule::State& state = parquetModule::current();
            *errorOut = state.loaded ? QStringLiteral("The Parquet module could not make a reader")
                                     : QStringLiteral("Parquet is not available: %1").arg(state.reason);
        }
        return false;
    }
    if (!reader->open(errorOut))
        return false;
    d->reader = std::move(reader);
    return true;
}

void ParquetTable::close()
{
    d->reader.reset();
}

bool ParquetTable::isOpen() const
{
    return d->reader != nullptr;
}

QString ParquetTable::fileSummary() const
{
    return d->reader ? d->reader->fileSummary() : QString();
}

qint64 ParquetTable::rowsDecoded() const
{
    return d->reader ? d->reader->rowsDecoded() : 0;
}

QStringList ParquetTable::columnTypes() const
{
    return d->reader ? d->reader->columnTypes() : QStringList();
}

QStringList ParquetTable::headers() const
{
    return d->reader ? d->reader->headers() : QStringList();
}

qint64 ParquetTable::totalRows() const
{
    return d->reader ? d->reader->totalRows() : 0;
}

qint64 ParquetTable::matchingRows(const QString& filter) const
{
    return d->reader ? d->reader->matchingRows(filter) : 0;
}

QList<QStringList> ParquetTable::rows(qint64 offset, int limit, const QString& filter, bool* readable) const
{
    if (!d->reader) {
        if (readable)
            *readable = true;
        return {};
    }
    return d->reader->rows(offset, limit, filter, readable);
}

QList<int> ParquetTable::columnWidths(int sampleRows) const
{
    return d->reader ? d->reader->columnWidths(sampleRows) : QList<int>();
}

} // namespace mole
