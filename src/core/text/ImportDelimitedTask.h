#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QChar>

namespace mole {

class DelimitedStore;

/// Streams a delimited file into a DelimitedStore.
///
/// The file is read in chunks and never held whole, so the memory cost is the
/// chunk plus one batch of rows regardless of whether the export is a kilobyte
/// or a hundred gigabytes.
class ImportDelimitedTask final : public Task
{
    Q_OBJECT

public:
    /// `store` is owned by the caller and must outlive the task.
    ImportDelimitedTask(
        FileSystemPtr fileSystem, VfsUri target, DelimitedStore* store, QObject* parent = nullptr);

    /// Null means detect from the first chunk.
    void setSeparator(QChar separator) { m_separator = separator; }
    void setFirstRowIsHeader(bool isHeader) { m_firstRowIsHeader = isHeader; }

    /// Valid once the task has succeeded.
    QChar separator() const { return m_separator; }
    qint64 importedRows() const { return m_importedRows; }

signals:
    /// Emitted as rows land, so a long import can be watched rather than
    /// waited on. Delivered on the receiver's thread like any other signal.
    void rowsImported(qint64 rows);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    DelimitedStore* m_store = nullptr;
    QChar m_separator;
    bool m_firstRowIsHeader = true;
    qint64 m_importedRows = 0;
};

} // namespace mole
