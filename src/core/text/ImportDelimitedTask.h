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
    /// The task holds a reference to `store` rather than a pointer to it. The
    /// interface lets go of the store the moment the reader moves to another
    /// file -- and this task may be halfway through a batch of inserts on a pool
    /// thread when that happens, because a cancellation is a flag it reads
    /// between chunks. Shared ownership makes that a write into a store nobody
    /// is reading rather than a write into freed memory. See MOLE-290.
    ImportDelimitedTask(FileSystemPtr fileSystem, VfsUri target, std::shared_ptr<DelimitedStore> store,
        QObject* parent = nullptr);

    /// Null means detect from the first chunk.
    void setSeparator(QChar separator) { m_separator = separator; }
    void setFirstRowIsHeader(bool isHeader) { m_firstRowIsHeader = isHeader; }

    /// Valid once the task has succeeded, or as soon as separatorDetected() has
    /// been delivered.
    QChar separator() const { return m_separator; }
    qint64 importedRows() const { return m_importedRows; }
    /// Whether any bytes could not be read as the encoding the head of the file
    /// said it was. The cells hold U+FFFD where they could not, and the status
    /// line says so -- an import that quietly replaced every accented character
    /// is worse than one that admits it. See MOLE-405.
    bool someBytesCouldNotBeRead() const { return m_undecodedBytes; }

signals:
    /// Emitted once the shape of the file has been settled, before the first
    /// row is stored. A view that shows rows while the import is still running
    /// would otherwise be captioned with a separator that was only a guess.
    void separatorDetected(QChar separator);
    /// Emitted as rows land, so a long import can be watched rather than
    /// waited on. Delivered on the receiver's thread like any other signal.
    void rowsImported(qint64 rows);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    std::shared_ptr<DelimitedStore> m_store;
    QChar m_separator;
    bool m_firstRowIsHeader = true;
    qint64 m_importedRows = 0;
    bool m_undecodedBytes = false;
};

} // namespace mole
