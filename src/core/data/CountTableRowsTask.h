#pragma once

#include "core/tasks/Task.h"

#include <QStringList>

namespace mole {

/// Counts the rows of every table in a SQLite file, on a pool thread.
///
/// `SELECT COUNT(*)` is a walk of the table however the file is indexed, so the
/// cost grows with the data and the interface thread must not wait for it --
/// which is exactly what opening a database used to do, once per table, before
/// it drew anything.
///
/// The task opens its own read-only connection. `connectionNameFor()` in
/// SqliteTable hashes the calling thread into the name, so a second connection
/// on a pool thread needs no new design: the interface reads through its own
/// while this one counts, the same shape the delimited importer already uses.
class CountTableRowsTask final : public Task
{
    Q_OBJECT

public:
    /// `tables` is counted in the order given, so a caller puts the table
    /// somebody is looking at in front of the ones they are not.
    CountTableRowsTask(QString path, QStringList tables, QObject* parent = nullptr);

signals:
    /// One table counted. Delivered on the receiver's thread, so a view can
    /// fill each count in as it lands rather than waiting for the last.
    void counted(const QString& table, qint64 rows);

protected:
    void run() override;

private:
    QString m_path;
    QStringList m_tables;
};

} // namespace mole
