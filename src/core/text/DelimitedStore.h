#pragma once

#include "core/data/ITableSource.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

class QSqlError;
class QSqlQuery;
class QTemporaryDir;

namespace mole {

/// A delimited file imported into SQLite so it can be paged, filtered and
/// sorted without ever holding it in memory.
///
/// The alternative -- parse the first N rows and stop -- means the viewer lies
/// about what is in the file, and a filter can only ever search the part that
/// happened to be loaded. Importing costs one pass; after that every question
/// is a query, and the answer covers the whole file however large it is.
///
/// One connection per thread, like the index: the importer writes from a pool
/// thread while the interface reads from its own -- and, like the index, in WAL
/// with a busy timeout on every one of them. Those two are not an optimisation;
/// they are what lets the grid be attached before the import starts. Without a
/// journal a write transaction holds the file exclusively, so a reader on the
/// drawing thread stalls behind a writer that commits every two thousand rows,
/// and whichever of them gives up first answers wrongly. See MOLE-289.
class DelimitedStore : public ITableSource
{
public:
    /// `path` is a file the caller owns and will delete. An empty path makes a
    /// private in-memory database, which only the creating thread can use.
    ///
    /// `scratch` is the temporary directory `path` lives in, when it is one, and
    /// the store keeps it alive. A store can outlive the interface that made it:
    /// an import the reader walked away from goes on writing until it notices,
    /// and a store whose file has been deleted from under it is no better off
    /// than a store that has been deleted itself. See MOLE-290.
    explicit DelimitedStore(QString path, std::shared_ptr<QTemporaryDir> scratch = {});
    ~DelimitedStore();

    DelimitedStore(const DelimitedStore&) = delete;
    DelimitedStore& operator=(const DelimitedStore&) = delete;

    bool open(QString* errorOut = nullptr);
    /// Closes and removes **every** connection the store handed out, not only
    /// the calling thread's. This is the end of the store's life rather than a
    /// pause in it: a store is closed when it is being destroyed, and which
    /// thread that happens on is whichever of the reader and the writer let go
    /// of it last.
    void close();
    bool isOpen() const { return m_open; }

    // ---- writing --------------------------------------------------------

    /// Starts a fresh import. Any previous contents are dropped.
    bool beginImport(const QStringList& headers, QString* errorOut = nullptr);
    /// Appends rows. Short rows are padded, long ones truncated, so a ragged
    /// file imports rather than failing halfway through.
    bool addRows(const QList<QStringList>& rows, QString* errorOut = nullptr);
    bool endImport(QString* errorOut = nullptr);

    // ---- reading --------------------------------------------------------

    QStringList headers() const override { return shape(); }
    /// Every row in the file.
    qint64 totalRows() const override;
    qint64 matchingRows(const QString& filter) const override;

    /// A window of rows in file order. This is the only read path the model
    /// uses, so scrolling costs one query per screen rather than one file.
    QList<QStringList> rows(
        qint64 offset, int limit, const QString& filter = {}, bool* readable = nullptr) const override;

    /// The longest value seen in each column over the first `sampleRows` rows,
    /// in characters. Used to size columns to their contents instead of a
    /// default that wastes half the window.
    QList<int> columnWidths(int sampleRows = 200) const override;

    /// What `PRAGMA name` reads back as on *this thread's* connection, or an
    /// empty string when there is no answer.
    ///
    /// Here because the settings that let a reader and a writer share the file
    /// are a property of a connection rather than of the store, and a test that
    /// read them from whichever connection open() happened to make would be
    /// asserting nothing about the one the importer writes through.
    QString pragmaValue(const QString& name) const;

private:
    QSqlDatabase connectionForCurrentThread() const;
    static QString columnName(int index);
    static QString whereClause(const QString& filter, int columns);
    static void bindFilter(QSqlQuery& query, const QString& filter);
    /// A locked database said as that, and anything else as the driver put it.
    static QString describe(const QSqlError& error);
    /// The columns, taken under the guard. Every read path wants them and the
    /// importer writes them from another thread.
    QStringList shape() const;

    QString m_path;
    /// The directory the file lives in, held only so that it outlives the store.
    /// Empty when the caller manages the path itself.
    std::shared_ptr<QTemporaryDir> m_scratch;
    /// Every connection name handed out, so close() can remove all of them. The
    /// importer's connection is made on a pool thread and used to be left behind
    /// whichever way the store ended.
    mutable QMutex m_connectionGuard;
    mutable QStringList m_connections;
    /// Guards m_headers alone: the importer settles the columns on a pool thread
    /// while the grid, already attached, is reading through the same store.
    mutable QMutex m_shapeGuard;
    QStringList m_headers;
    bool m_open = false;
    /// Cached because every scroll step asks for it and it never changes
    /// after an import. Atomic because the importer invalidates it from its own
    /// thread on every batch while the interface is reading it.
    mutable std::atomic<qint64> m_totalRows { -1 };
};

} // namespace mole
