#pragma once

#include "core/sync/SyncOptions.h"
#include "core/vfs/IFileSystem.h"

#include <QList>

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
    };

    /// Compares the two trees. Both sides are read through the VFS, so this
    /// works between any pair of drives -- including ones that do not exist yet.
    static SyncPlan build(IFileSystem* sourceFs, const VfsUri& source, IFileSystem* targetFs,
        const VfsUri& target, const SyncOptions& options, const CancelToken& cancel);

    const QList<Step>& steps() const { return m_steps; }
    int countOf(Action action) const;
    qint64 bytesToTransfer() const;
    bool isEmpty() const { return m_steps.isEmpty(); }

    static QString actionLabel(Action action);

private:
    QList<Step> m_steps;
};

} // namespace mole
