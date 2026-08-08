#pragma once

#include "core/duplicates/DuplicateStrategy.h"
#include "core/tasks/Task.h"

#include <QList>

namespace mole {

class VfsManager;

/// A set of files the strategy considers the same.
struct DuplicateGroup
{
    /// The key they all agreed on at the last stage, for debugging and display.
    QString key;
    QList<FileEntry> files;
    /// Bytes that could be freed by keeping one and removing the rest.
    qint64 reclaimable = 0;
};

/// Walks one or more trees and groups the files a strategy calls duplicates.
///
/// The staged design is the point: each stage narrows the candidates, and only
/// groups that survive reach the next one. Hashing an entire tree to find
/// duplicates is the obvious approach and is far slower than grouping by size
/// first -- on a NAS it is the difference between minutes and hours.
class FindDuplicatesTask final : public Task
{
    Q_OBJECT

public:
    FindDuplicatesTask(VfsManager* vfs, QList<VfsUri> roots, std::unique_ptr<IDuplicateStrategy> strategy,
        QObject* parent = nullptr);
    ~FindDuplicatesTask() override;

    /// Ignores files smaller than this. Most trees contain thousands of tiny
    /// identical files nobody wants listed.
    void setMinimumSize(qint64 bytes) { m_minimumSize = bytes; }

    /// Valid once finished. Largest reclaimable first, because that is the
    /// order anybody clearing space wants them in.
    QList<DuplicateGroup> groups() const { return m_groups; }
    qint64 reclaimableBytes() const { return m_reclaimable; }

signals:
    void groupsReady(const QList<mole::DuplicateGroup>& groups);

protected:
    void run() override;

private:
    VfsManager* m_vfs = nullptr;
    QList<VfsUri> m_roots;
    std::unique_ptr<IDuplicateStrategy> m_strategy;
    qint64 m_minimumSize = 1;
    QList<DuplicateGroup> m_groups;
    qint64 m_reclaimable = 0;
};

} // namespace mole

Q_DECLARE_METATYPE(QList<mole::DuplicateGroup>)
