#include "core/sync/SyncPlan.h"

#include "core/duplicates/ContentComparison.h"

#include <QHash>

#include <algorithm>

namespace mole {
namespace {

    /// Everything in one directory, keyed by name, and whether it could be read
    /// at all.
    ///
    /// The difference matters entirely at the source. An empty directory and one
    /// that could not be listed look identical in the result, and in a mirror
    /// that is the difference between "the source has nothing here" and "we
    /// could not see" -- the first of which is an instruction to delete
    /// everything at the far end. At the destination the two really are the
    /// same: a target that does not exist yet reads as empty, which is exactly
    /// right for a first sync.
    struct Listing
    {
        QHash<QString, FileEntry> byName;
        bool readable = true;
    };

    Listing listByName(IFileSystem* fs, const VfsUri& dir, const CancelToken& cancel)
    {
        Listing out;
        if (!fs) {
            out.readable = false;
            return out;
        }
        Result<FileEntryList> listed = fs->list(dir, cancel);
        if (!listed.ok()) {
            // Cancellation is not a fault in the source, but it is still not a
            // picture of it, and the caller must not act on the difference.
            out.readable = false;
            return out;
        }
        for (const FileEntry& entry : listed.value())
            out.byName.insert(entry.name, entry);
        return out;
    }

    /// What a contents comparison can come back with. Three answers and not two:
    /// a file nobody could read is not a file that differs, and treating it as
    /// one is how a sync overwrites something it could not look at.
    enum class Contents { Same, Different, Unreadable };

    /// Whether two files of the same size hold the same bytes.
    ///
    /// The same path the duplicates scan settles a group with -- see ADR-0046 for
    /// the measurements. Qt 6.4 carries its own SHA-2 and does not use the
    /// processor's SHA-NI instructions, so hashing runs at about 218 MB/s whatever
    /// the storage is, against 87 GB/s for a memcmp: a sync by contents over local
    /// disks used to wait for a core rather than for the disk. Each file is still
    /// read exactly once, and two that differ now stop at the first chunk that
    /// differs, which in a sync is the common case -- the interesting files are
    /// the ones that differ.
    ///
    /// partitionByContents() leaves a file it could not open out of its result
    /// altogether, deliberately: an unreadable file is not a match for every
    /// other unreadable file. So fewer than two files across the groups it hands
    /// back is that condition rather than a difference.
    Contents compareContents(IFileSystem* sourceFs, const FileEntry& source, IFileSystem* targetFs,
        const FileEntry& target, const CancelToken& cancel)
    {
        if (!sourceFs || !targetFs)
            return Contents::Unreadable;

        // The two sides are told apart by their uri, which is the only thing that
        // distinguishes them here -- and a sync of a tree onto itself, where they
        // are the same file, wants the same answer either way.
        const VfsUri sourceUri = source.uri;
        const DriveLookup driveFor = [&](const FileEntry& entry) -> IFileSystem* {
            return entry.uri == sourceUri ? sourceFs : targetFs;
        };

        const QList<QList<FileEntry>> groups = partitionByContents({ source, target }, driveFor, cancel);
        int compared = 0;
        for (const QList<FileEntry>& group : groups)
            compared += static_cast<int>(group.size());
        if (compared < 2)
            return Contents::Unreadable;
        return groups.size() == 1 ? Contents::Same : Contents::Different;
    }

    /// Whether the destination copy needs replacing, and why.
    QString differenceBetween(const FileEntry& source, const FileEntry& target, const SyncOptions& options,
        IFileSystem* sourceFs, IFileSystem* targetFs, const CancelToken& cancel)
    {
        switch (options.compare) {
        case SyncOptions::Compare::SizeOnly:
            return source.size != target.size ? QStringLiteral("size differs") : QString();
        case SyncOptions::Compare::Contents: {
            // Two files of different sizes are settled without opening anything.
            if (source.size != target.size)
                return QStringLiteral("size differs");
            switch (compareContents(sourceFs, source, targetFs, target, cancel)) {
            case Contents::Same:
                return {};
            case Contents::Different:
                return QStringLiteral("contents differ");
            case Contents::Unreadable:
                return QStringLiteral("could not be compared");
            }
            return QStringLiteral("could not be compared");
        }
        case SyncOptions::Compare::SizeAndTime:
            break;
        }

        if (source.size != target.size)
            return QStringLiteral("size differs");
        // A second of slack: filesystems disagree about sub-second precision, and
        // without it every sync between two of them copies everything, every time.
        if (source.modified.isValid() && target.modified.isValid()
            && std::llabs(target.modified.secsTo(source.modified)) > 1) {
            return QStringLiteral("modified at a different time");
        }
        return {};
    }

    void walk(IFileSystem* sourceFs, const VfsUri& source, IFileSystem* targetFs, const VfsUri& target,
        const SyncOptions& options, const QString& relative, const CancelToken& cancel,
        QList<SyncPlan::Step>& steps, QStringList& unreadable)
    {
        if (cancel.isCancelled())
            return;

        const Listing sourceListing = listByName(sourceFs, source, cancel);
        if (!sourceListing.readable) {
            // Nothing is planned for a directory we could not read -- not a copy,
            // and above all not a deletion. A source that cannot be seen is not a
            // source that is empty, and a mirror told the difference wrongly
            // deletes the only remaining copy of everything in it.
            if (!cancel.isCancelled())
                unreadable.append(source.path());
            return;
        }

        const QHash<QString, FileEntry>& here = sourceListing.byName;
        const QHash<QString, FileEntry> there = listByName(targetFs, target, cancel).byName;

        for (auto it = here.constBegin(); it != here.constEnd(); ++it) {
            if (cancel.isCancelled())
                return;

            const FileEntry& entry = it.value();
            const QString path = relative.isEmpty() ? entry.name : relative + QLatin1Char('/') + entry.name;

            if (!options.accepts(entry.name, entry.isHidden)) {
                steps.append(SyncPlan::Step { SyncPlan::Action::Skip, entry.uri, target.child(entry.name),
                    path, entry.size, QStringLiteral("filtered out") });
                continue;
            }

            if (entry.isDir) {
                if (!options.recursive)
                    continue;
                if (!there.contains(entry.name)) {
                    steps.append(SyncPlan::Step { SyncPlan::Action::CreateDirectory, entry.uri,
                        target.child(entry.name), path, 0, QStringLiteral("not there") });
                }
                walk(sourceFs, entry.uri, targetFs, target.child(entry.name), options, path, cancel, steps,
                    unreadable);
                continue;
            }

            const VfsUri destination = target.child(entry.name);

            if (!there.contains(entry.name)) {
                steps.append(SyncPlan::Step { SyncPlan::Action::Copy, entry.uri, destination, path,
                    entry.size, QStringLiteral("not there") });
                continue;
            }

            if (options.mode == SyncOptions::Mode::FillGaps) {
                continue; // already there; this mode never looks further
            }

            const FileEntry& existing = there.value(entry.name);

            // A destination newer than its source is usually work done at the far
            // end, and overwriting it is the mistake this guard exists for.
            if (options.skipNewer && existing.modified.isValid() && entry.modified.isValid()
                && existing.modified > entry.modified.addSecs(1)) {
                steps.append(SyncPlan::Step { SyncPlan::Action::Skip, entry.uri, destination, path,
                    entry.size, QStringLiteral("newer at the destination") });
                continue;
            }

            const QString difference
                = differenceBetween(entry, existing, options, sourceFs, targetFs, cancel);
            if (!difference.isEmpty()) {
                steps.append(SyncPlan::Step {
                    SyncPlan::Action::Overwrite, entry.uri, destination, path, entry.size, difference });
            }
        }

        // Only a mirror removes anything, and only what the source does not have.
        if (options.mode != SyncOptions::Mode::Mirror)
            return;

        for (auto it = there.constBegin(); it != there.constEnd(); ++it) {
            if (cancel.isCancelled())
                return;
            if (here.contains(it.key()))
                continue;
            // A filtered-out name was never considered for copying, so deleting it
            // at the far end would be acting on a rule the user did not give.
            if (!options.accepts(it.key(), it.value().isHidden))
                continue;

            const QString path = relative.isEmpty() ? it.key() : relative + QLatin1Char('/') + it.key();
            steps.append(SyncPlan::Step { SyncPlan::Action::Delete, VfsUri {}, it.value().uri, path,
                it.value().size, QStringLiteral("not in the source") });
        }
    }

} // namespace

SyncPlan SyncPlan::build(IFileSystem* sourceFs, const VfsUri& source, IFileSystem* targetFs,
    const VfsUri& target, const SyncOptions& options, const CancelToken& cancel)
{
    SyncPlan plan;
    if (!sourceFs || !targetFs)
        return plan;
    walk(sourceFs, source, targetFs, target, options, QString(), cancel, plan.m_steps, plan.m_unreadable);

    // Directories before the files that go in them, deletions last: a mirror
    // that deleted first would remove a file it was about to be given back.
    std::stable_sort(plan.m_steps.begin(), plan.m_steps.end(), [](const Step& a, const Step& b) {
        const auto rank = [](Action action) {
            switch (action) {
            case Action::CreateDirectory:
                return 0;
            case Action::Copy:
            case Action::Overwrite:
                return 1;
            case Action::Skip:
                return 2;
            case Action::Delete:
                return 3;
            }
            return 4;
        };
        return rank(a.action) < rank(b.action);
    });
    return plan;
}

int SyncPlan::countOf(Action action) const
{
    int count = 0;
    for (const Step& step : m_steps) {
        if (step.action == action)
            ++count;
    }
    return count;
}

qint64 SyncPlan::bytesToTransfer() const
{
    qint64 bytes = 0;
    for (const Step& step : m_steps) {
        if (step.action == Action::Copy || step.action == Action::Overwrite)
            bytes += step.bytes;
    }
    return bytes;
}

QString SyncPlan::actionLabel(Action action)
{
    switch (action) {
    case Action::CreateDirectory:
        return QStringLiteral("new folder");
    case Action::Copy:
        return QStringLiteral("copy");
    case Action::Overwrite:
        return QStringLiteral("replace");
    case Action::Delete:
        return QStringLiteral("delete");
    case Action::Skip:
        return QStringLiteral("skip");
    }
    return {};
}

} // namespace mole
