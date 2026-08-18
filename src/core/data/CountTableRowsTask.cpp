#include "core/data/CountTableRowsTask.h"

#include "core/data/SqliteTable.h"
#include "core/vfs/VfsTypes.h"

#include <QFileInfo>

namespace mole {

CountTableRowsTask::CountTableRowsTask(QString path, QStringList tables, QObject* parent)
    : Task(QStringLiteral("Counting rows in %1").arg(QFileInfo(path).fileName()), parent)
    , m_path(std::move(path))
    , m_tables(std::move(tables))
{
    // Nobody asked for this: they asked to look at a database, and the counts
    // are detail filled in behind the view. It is still cancelled and awaited
    // like anything else -- it is only kept out of the strip, which belongs to
    // the work the user did ask for.
    setBackground(true);
}

void CountTableRowsTask::run()
{
    if (m_tables.isEmpty())
        return;

    // Its own connection on this thread, opened read-only like the one the
    // interface reads through. Previewing a file is not a licence to modify it,
    // and that holds for the connection that only counts.
    SqliteTable database(m_path);
    QString error;
    if (!database.open(&error)) {
        fail(VfsError::make(VfsError::Unknown, error));
        return;
    }

    int done = 0;
    for (const QString& table : std::as_const(m_tables)) {
        if (isCancelRequested())
            return;

        const qint64 rows = database.rowCountOf(table);
        emit counted(table, rows);

        // Per table rather than per row: a count is one query that either has
        // answered or has not, so there is no middle to report.
        setStatusText(table);
        setProgress(100 * ++done / static_cast<int>(m_tables.size()));
    }
}

} // namespace mole
