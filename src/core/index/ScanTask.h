#pragma once

#include "core/index/IndexDatabase.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <functional>

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
    /// How many files the scan read anything out of, which is where the time
    /// goes when it is asked to.
    qint64 filesRead() const { return m_filesRead; }

    /// What the files say about themselves, recorded alongside them.
    ///
    /// Off by default and stated per scan, because the cost is bounded per file
    /// and unbounded in aggregate: a hundred thousand photographs is a hundred
    /// thousand reads. A scan without it writes exactly what a scan wrote
    /// before any of this existed. See ADR-0039.
    void setFactReader(std::function<QList<SearchFact>(const FileEntry&)> reader);

protected:
    void run() override;

private:
    static constexpr int kBatchSize = 2000;

    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    QString m_label;
    IndexDatabase* m_index = nullptr;
    qint64 m_filesIndexed = 0;
    qint64 m_filesRead = 0;
    qint64 m_skippedDirectories = 0;
    std::function<QList<SearchFact>(const FileEntry&)> m_facts;
};

} // namespace mole
