#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QJsonObject>
#include <QStringList>

#include <memory>

namespace mole {

class DelimitedStore;

/// Streams a file of JSON records into a DelimitedStore, one record per row.
///
/// `ImportDelimitedTask` with a different parser, and deliberately so: the store
/// holds rows of text under a fixed list of headers, and the only thing about it
/// that is delimited-text specific is its name. So the shape here is the same --
/// 1 MB chunks, 5,000-row batches, nothing held whole -- and a forty gigabyte
/// export costs a chunk and a batch exactly as a CSV of that size does.
///
/// **The columns are the keys, settled from the head of the file.** The union of
/// the top-level keys of every record in the first `kSampleBytes`, in the order
/// they are first seen, held for the rest of the file. That is the CSV importer's
/// own rule and following it beats inventing a second answer to the same
/// question -- but it costs something here that it does not cost there, because a
/// JSON record is free to carry a key no earlier record had. A key first seen
/// after the sample gets no column and its values are not in the grid; they are
/// counted instead, so the reader is told rather than left to notice.
///
/// Growing the table as new keys turn up was the alternative. SQLite adds a
/// column in metadata alone, so it is cheap; it is rejected because it changes
/// DelimitedStore for one caller, because it moves the columns under a reader who
/// is already scrolling, and because a file with an unbounded variety of keys
/// would walk into SQLite's column limit with nothing in the way.
class ImportJsonLinesTask final : public Task
{
    Q_OBJECT

public:
    /// The task holds a reference to `store` rather than a pointer to it. The
    /// interface lets go of the store the moment the reader moves to another
    /// file -- and this task may be halfway through a batch of inserts on a pool
    /// thread when that happens, because a cancellation is a flag it reads
    /// between chunks. Shared ownership makes that a write into a store nobody
    /// is reading rather than a write into freed memory. See MOLE-290.
    ImportJsonLinesTask(FileSystemPtr fileSystem, VfsUri target, std::shared_ptr<DelimitedStore> store,
        QObject* parent = nullptr);

    /// How much of the file the columns are decided from. The delimited
    /// importer's own sample, for the same reason: re-guessing further down would
    /// silently change the shape of the table halfway through it.
    static constexpr int kSampleBytes = 64 * 1024;

    /// The columns, once the shape has been settled. Empty before that.
    QStringList headers() const { return m_headers; }
    qint64 importedRows() const { return m_importedRows; }
    /// Lines that were not a JSON object: malformed JSON, and valid JSON that is
    /// a number, a string or an array. Counted and not shown -- refusing the file
    /// outright would leave it unviewable, which is the reasoning that pads a
    /// ragged CSV row rather than rejecting it.
    qint64 skippedLines() const { return m_skippedLines; }
    /// Keys first seen after the sample, so they have no column of their own.
    /// The count, because the names of them are unbounded and the figure is what
    /// a reader acts on.
    qint64 keysWithoutAColumn() const { return m_keysWithoutAColumn; }

    /// Whether anything in the sample was a JSON object at all.
    ///
    /// False for a pretty-printed document under the wrong name, a file of JSON
    /// arrays, or something that is not JSON -- and then there is no table to
    /// show and the viewer shows the source instead. Distinct from an import that
    /// failed: the file was read perfectly well and is not a list of records.
    bool looksLikeRecords() const { return m_looksLikeRecords; }

signals:
    /// The shape, as soon as it is settled and before the first row is stored, so
    /// a grid filling while the import runs is captioned with the real columns.
    void shapeSettled();
    /// Rows as they land, so a long import can be watched rather than waited on.
    void rowsImported(qint64 rows);

protected:
    void run() override;

private:
    /// The union of the top-level keys of the records in `sample`, in first-seen
    /// order. Static and testable: it is the one rule here that decides what the
    /// reader sees, and it depends on nothing but the text.
    static QStringList keysIn(const QString& sample, bool* sawAnObject);

    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    std::shared_ptr<DelimitedStore> m_store;
    QStringList m_headers;
    qint64 m_importedRows = 0;
    qint64 m_skippedLines = 0;
    qint64 m_keysWithoutAColumn = 0;
    bool m_looksLikeRecords = false;
};

/// One record as one row, under `headers`.
///
/// **A cell holds one value, and a nested value holds its JSON.** A string is
/// itself, a number or a boolean is its JSON text, `null` is `null`, and a key
/// the record does not have is an empty cell -- the difference between absent and
/// null is worth keeping. An object or an array becomes its *compact* JSON, one
/// line, no indentation.
///
/// That is the whole answer to a file with complicated structures in it: nesting
/// never breaks the grid, it makes a cell with JSON in it, and because the filter
/// is a substring over every column, searching still finds what is inside one.
/// Flattening `user.name` into a column of its own is a real thing to want and is
/// deliberately not here: an array has no key to flatten, and one nested list
/// would multiply the columns of the whole table.
///
/// Free of the task so it can be tested without a file, and so the viewer and the
/// importer cannot disagree about what a cell holds. `unseen` counts keys the
/// record has that `headers` does not.
QStringList jsonRecordAsRow(const QJsonObject& record, const QStringList& headers, qint64* unseen = nullptr);

/// The top-level keys of one record, in the order the text writes them.
///
/// **QJsonObject sorts its keys**, so the file's own order is not recoverable
/// from the parsed value -- and the order a record was written in is information
/// about the file, which is why the columns follow it rather than the alphabet.
/// So the text is scanned for the names at depth one.
///
/// The scan is used only to *order* keys `record` already has. It cannot
/// introduce one, and a key it fails to find falls to the end instead of
/// disappearing, so a line the scanner reads badly costs column order and never
/// a column. Exposed for its own test: it is a parser, and a parser with no test
/// is a guess.
QStringList jsonKeysInTextOrder(const QString& line, const QJsonObject& record);

} // namespace mole
