#pragma once

#include "core/vfs/IFileSystem.h"

#include <QList>
#include <QString>

#include <memory>

namespace mole {

/// How two files are decided to be the same.
///
/// Expressed as ordered *stages* rather than one comparison, because that is the
/// shape the problem actually has: every worthwhile strategy starts with
/// something cheap that can rule most files out, and only then pays for
/// something expensive on what is left. Hashing every file in a tree to find
/// duplicates is the naive approach and is orders of magnitude slower than
/// grouping by size first.
///
/// A stage produces a key. Files whose keys differ at any stage are not
/// duplicates and are never compared again; files that agree all the way through
/// are a group. Adding a strategy -- perceptual image hashing, tag comparison,
/// whatever comes later -- means implementing this and nothing else.
class IDuplicateStrategy
{
public:
    virtual ~IDuplicateStrategy() = default;

    virtual QString id() const = 0;
    virtual QString label() const = 0;
    /// One sentence on what it matches and what it costs, shown in the picker.
    /// The cost is the part people need before they start a scan on a NAS.
    virtual QString description() const = 0;

    /// Names of the stages, cheapest first. Used for progress and to explain
    /// what a scan is doing.
    virtual QStringList stageNames() const = 0;
    int stageCount() const { return static_cast<int>(stageNames().size()); }

    /// Whether a stage has to read the file rather than its metadata. A stage
    /// that does is only ever run on candidates that survived the earlier ones.
    virtual bool stageReadsContent(int stage) const = 0;

    /// The key for one file at one stage. An empty key means "cannot be
    /// compared", and the file is dropped rather than grouped with everything
    /// else that failed for its own unrelated reason.
    virtual QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const
        = 0;

    /// Every strategy this build offers, in the order the picker shows them.
    static std::vector<std::unique_ptr<IDuplicateStrategy>> all();
};

} // namespace mole
