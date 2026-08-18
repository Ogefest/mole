#pragma once

#include "core/vfs/IFileSystem.h"

#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace mole {

/// How to reach the drive a file lives on. A bucket can hold files from more
/// than one of them, because a scan can be given roots on several drives.
using DriveLookup = std::function<IFileSystem*(const FileEntry&)>;

/// How two files are decided to be the same.
///
/// Expressed as ordered *stages* rather than one comparison, because that is the
/// shape the problem actually has: every worthwhile strategy starts with
/// something cheap that can rule most files out, and only then pays for
/// something expensive on what is left. Hashing every file in a tree to find
/// duplicates is the naive approach and is orders of magnitude slower than
/// grouping by size first.
///
/// A stage settles a bucket one of two ways, and says which through
/// stageComparesContent():
///
/// - **By key.** Every file is given one, and files whose keys differ are not
///   duplicates and are never compared again. Cheap, independent per file, and
///   what every stage but the last one wants.
/// - **By comparison.** The files of a bucket are compared with one another
///   directly. A last stage that has to *prove* something wants this: a key is a
///   claim about a file on its own, and proving two files identical from two
///   keys means trusting that no two different files share one.
///
/// Files that agree all the way through are a group. Adding a strategy --
/// perceptual image hashing, tag comparison, whatever comes later -- means
/// implementing this and nothing else.
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
    ///
    /// Never called for a stage that compares -- see below.
    virtual QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const
        = 0;

    /// Whether this stage settles a bucket by comparing its files with one
    /// another rather than by giving each of them a key. A stage that does gets
    /// compare() instead of keyFor().
    virtual bool stageComparesContent(int) const { return false; }

    /// Splits `bucket` into the groups this stage considers the same. Called
    /// only for a stage that says it compares, and only with files that have
    /// agreed at every earlier stage. Every group is returned, the ones holding
    /// a single file included: it is the scan that decides a lone file is not a
    /// duplicate.
    virtual QList<QList<FileEntry>> compare(int stage, const QList<FileEntry>& bucket,
        const DriveLookup& driveFor, const CancelToken& cancel) const;

    /// Every strategy this build offers, in the order the picker shows them.
    static std::vector<std::unique_ptr<IDuplicateStrategy>> all();
};

} // namespace mole
