#pragma once

#include "core/data/ITableSource.h"

#include <memory>

namespace mole {

/// A Parquet file read in place.
///
/// Parquet is columnar and stores its rows in groups, each with its own
/// statistics, so a windowed read is cheap: only the groups a window touches are
/// decoded. That is what lets a multi-gigabyte file open at once, the same
/// property that makes reading SQLite in place worthwhile.
///
/// Compiled only when Arrow is available. Without it `isSupported()` is false and
/// the preview falls through to the file-information viewer -- a missing optional
/// library must not stop the application being built.
class ParquetTable : public ITableSource
{
public:
    explicit ParquetTable(QString path);
    ~ParquetTable() override;

    ParquetTable(const ParquetTable&) = delete;
    ParquetTable& operator=(const ParquetTable&) = delete;

    /// Whether this build can read Parquet at all.
    static bool isSupported();

    bool open(QString* errorOut = nullptr);
    void close();
    bool isOpen() const;

    /// What the file says about itself, for the header strip: row groups,
    /// compression, the writer that produced it.
    QString fileSummary() const;

    /// Rows Arrow has decoded for this file since it was opened.
    ///
    /// Here to be asserted on. A read that decodes a whole row group to show
    /// fifty rows gives exactly the same answer as one that decodes a batch, so
    /// the bound is invisible in the rows themselves -- and a test that timed it
    /// instead would be a test that fails on a busy machine. See MOLE-287.
    qint64 rowsDecoded() const;
    /// Column types as Arrow reports them, for the tooltip on each header.
    QStringList columnTypes() const;

    // ---- ITableSource ---------------------------------------------------

    QStringList headers() const override;
    /// True: a Parquet file is read through Arrow, which knows nothing of the
    /// thread it is called on, and nothing here is bound to one. The grid reads it
    /// on a task because a windowed read of this format is not bounded by its
    /// arguments the way a SQL one is -- see ITableSource::canBeReadOnATask().
    bool canBeReadOnATask() const override { return true; }
    qint64 totalRows() const override;
    qint64 matchingRows(const QString& filter) const override;
    QList<QStringList> rows(
        qint64 offset, int limit, const QString& filter = {}, bool* readable = nullptr) const override;
    QList<int> columnWidths(int sampleRows = 200) const override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace mole
