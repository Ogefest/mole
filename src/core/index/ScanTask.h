#pragma once

#include "core/index/IndexDatabase.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Walks a tree and writes it into the index so it can be searched later
/// without touching the (possibly remote, possibly slow) filesystem again.
class ScanTask final : public Task
{
    Q_OBJECT

public:
    /// `index` is borrowed and must outlive the task.
    ScanTask(FileSystemPtr fileSystem, VfsUri root, QString label, IndexDatabase* index,
        QObject* parent = nullptr);

    qint64 filesIndexed() const { return m_filesIndexed; }
    qint64 skippedDirectories() const { return m_skippedDirectories; }

protected:
    void run() override;

private:
    static constexpr int kBatchSize = 2000;

    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    QString m_label;
    IndexDatabase* m_index = nullptr;
    qint64 m_filesIndexed = 0;
    qint64 m_skippedDirectories = 0;
};

} // namespace mole
