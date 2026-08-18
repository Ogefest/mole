#pragma once

#include "core/duplicates/DuplicateStrategy.h"
#include "core/tasks/Task.h"

#include <QList>

#include <functional>

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

    /// Every group confirmed so far, largest reclaimable first -- because that is
    /// the order anybody clearing space wants them in.
    ///
    /// Readable while the scan is still running, and sorted at every instant
    /// rather than only at the end: a group is inserted in its place as it is
    /// confirmed. See docs/adr/0043-a-duplicate-group-is-reported-when-it-is-
    /// confirmed.md for why rows moving beat a list that is in arrival order for
    /// the whole of a scan and then jumps.
    QList<DuplicateGroup> groups() const { return m_groups; }
    qint64 reclaimableBytes() const { return m_reclaimable; }
    /// Files that changed while they were being compared, and were therefore
    /// left out of every group. Reported rather than hidden: a scan that quietly
    /// ignored a file is a scan whose answer is smaller than the truth.
    int changedDuringTheScan() const { return m_movedUnderfoot; }

signals:
    /// One group, confirmed: it has agreed at every stage, so nothing later in
    /// this scan can withdraw it or add to it.
    ///
    /// Emitted as the scan runs rather than at the end. A row that appears and
    /// then vanishes is worse than a row that appears late -- it teaches people
    /// not to believe the list -- so nothing goes out until it is settled.
    ///
    /// `position` is where it belongs in groups(), which is kept in order as it
    /// fills. Handed over rather than left to be worked out again, because the
    /// task has just worked it out.
    void groupFound(const mole::DuplicateGroup& group, int position);

protected:
    void run() override;

private:
    /// Splits `bucket` by the key `stage` gives each file, dropping whatever is
    /// left alone. `examined` and `total` are carried through only to say what the
    /// scan is doing.
    QList<QList<FileEntry>> splitAtStage(const QList<FileEntry>& bucket, int stage,
        const std::function<IFileSystem*(const FileEntry&)>& driveFor, int& examined, int total);
    /// Files that agreed all the way through are a group. Inserted in its place
    /// and announced.
    void confirm(const QList<FileEntry>& files);

    VfsManager* m_vfs = nullptr;
    QList<VfsUri> m_roots;
    std::unique_ptr<IDuplicateStrategy> m_strategy;
    qint64 m_minimumSize = 1;
    QList<DuplicateGroup> m_groups;
    qint64 m_reclaimable = 0;
    int m_movedUnderfoot = 0;
};

} // namespace mole

// Crosses a thread boundary one at a time now as well as in a list: a group is
// confirmed on a pool thread and the tab that shows it lives on the other one.
Q_DECLARE_METATYPE(mole::DuplicateGroup)
Q_DECLARE_METATYPE(QList<mole::DuplicateGroup>)
