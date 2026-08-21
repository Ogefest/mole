#pragma once

#include "core/rename/RenameRule.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/VfsUri.h"

#include <QList>

namespace mole {

/// What a set of rules would do to a set of names, worked out before anything
/// happens.
///
/// The preview is the feature. Renaming two hundred files on faith is how people
/// lose an evening, and a batch that half-succeeds is worse than one that never
/// ran -- so a plan that would collide is refused whole rather than discovered
/// halfway through.
class RenamePlan
{
public:
    /// One file's before and after.
    struct Entry
    {
        VfsUri source;
        QString originalName;
        QString newName;
        /// Why this row cannot be renamed, or empty when it can.
        QString problem;
        /// A name the destination would accept, when the problem is the name
        /// itself. Offered, never applied: a file that arrives under a name
        /// nobody chose is harder to find later than one that did not arrive.
        QString suggestion;

        bool changed() const { return newName != originalName; }
        bool isBlocked() const { return !problem.isEmpty(); }
    };

    /// Applies `rules` to `sources` in order. `existingNames` is what is already
    /// in each directory, so a rename onto something that is there is caught
    /// here rather than by the filesystem halfway through.
    /// `sensitivity` is what the destination volume does about case, and it has
    /// to be the backend's answer rather than a guess: whatever the backend
    /// decides is a collision is what this has to predict, or the preview says a
    /// plan is fine that the filesystem will refuse -- and the whole point of the
    /// bulk rename tool is that somebody can trust the preview before touching a
    /// hundred files.
    /// `names` is what the drive will accept in a name, and it is the other
    /// thing the preview has to know: the bulk rename tool exists so somebody
    /// can trust the preview before touching a hundred files, and a plan full of
    /// clean rows that the filesystem will refuse is worse than no preview.
    static RenamePlan build(const QList<VfsUri>& sources, const QList<RenameRule>& rules,
        const QHash<QString, QStringList>& existingNames = {},
        Qt::CaseSensitivity sensitivity = Qt::CaseSensitive, const NameRules& names = {});

    const QList<Entry>& entries() const { return m_entries; }
    int changedCount() const;
    int blockedCount() const;
    /// True when every row is either unchanged or safely renameable.
    bool canApply() const { return blockedCount() == 0 && changedCount() > 0; }

    /// Applies the rules to one name. Exposed because each rule is worth testing
    /// on its own, without a filesystem anywhere near it.
    static QString apply(const QString& name, const QList<RenameRule>& rules, int index);

private:
    QList<Entry> m_entries;
};

} // namespace mole
