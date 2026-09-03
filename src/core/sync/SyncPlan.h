#pragma once

#include "core/sync/SyncOptions.h"
#include "core/vfs/IFileSystem.h"

#include <QDateTime>
#include <QList>
#include <QStringList>

namespace mole {

/// What a sync would do, worked out before it does any of it.
///
/// Built identically whether or not it will be carried out, which is what makes
/// a dry run trustworthy: it is not a simulation of the real path, it *is* the
/// real path with the last step withheld.
class SyncPlan
{
public:
    enum class Action {
        CreateDirectory,
        Copy, ///< not at the destination
        Overwrite, ///< there and different
        Delete, ///< at the destination and not in the source
        Skip ///< matched a rule that says leave it alone
    };

    struct Step
    {
        Action action = Action::Copy;
        VfsUri source;
        VfsUri target;
        QString relativePath;
        qint64 bytes = 0;
        /// Why, in words: "newer at the destination", "size differs". Shown in
        /// the preview, because a list of file names explains nothing.
        QString reason;
        /// What stood at the destination when the plan was made, for a step
        /// that will remove it. A plan is agreed to and then carried out, and
        /// between those two moments the destination can gain a file -- under
        /// the very name that is going, or inside the folder about to be removed
        /// with everything in it. A deletion is the one step here that cannot be
        /// undone, so it is the one step that asks again before acting. Invalid
        /// where the drive does not report a time, which is answer enough: the
        /// check then rests on the size alone.
        ///
        /// Last in the struct on purpose: every step is built by aggregate
        /// initialisation, and only a deletion has anything to say here.
        QDateTime targetModified;
    };

    /// Compares the two trees. Both sides are read through the VFS, so this
    /// works between any pair of drives -- including ones that do not exist yet.
    static SyncPlan build(IFileSystem* sourceFs, const VfsUri& source, IFileSystem* targetFs,
        const VfsUri& target, const SyncOptions& options, const CancelToken& cancel);

    const QList<Step>& steps() const { return m_steps; }
    /// Directories the comparison could not read, in words. A plan with any of
    /// these in it is a plan built from an incomplete picture of the source, and
    /// the parts it could not see produced no steps at all -- neither copies nor
    /// deletions. See ADR-0030.
    const QStringList& unreadable() const { return m_unreadable; }
    int countOf(Action action) const;
    qint64 bytesToTransfer() const;
    bool isEmpty() const { return m_steps.isEmpty(); }

    static QString actionLabel(Action action);

private:
    QList<Step> m_steps;
    QStringList m_unreadable;
};

} // namespace mole
