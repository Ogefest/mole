#include "core/sync/SyncPlan.h"

#include <QCryptographicHash>
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

    /// Hashes a file for a contents comparison. An unreadable file gets an empty
    /// hash, which never equals anything -- so it is treated as different and
    /// copied, rather than silently assumed to match.
    QString hashOf(IFileSystem* fs, const VfsUri& uri, const CancelToken& cancel)
    {
        if (!fs)
            return {};
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(uri);
        if (!opened.ok())
            return {};
        std::unique_ptr<QIODevice> device = std::move(opened.value());
        if (!device)
            return {};

        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!device->atEnd()) {
            if (cancel.isCancelled())
                return {};
            const QByteArray chunk = device->read(256 * 1024);
            if (chunk.isEmpty())
                break;
            hash.addData(chunk);
        }
        return QString::fromLatin1(hash.result().toHex());
    }

    /// Whether the destination copy needs replacing, and why.
    QString differenceBetween(const FileEntry& source, const FileEntry& target, const SyncOptions& options,
        IFileSystem* sourceFs, IFileSystem* targetFs, const CancelToken& cancel)
    {
        switch (options.compare) {
        case SyncOptions::Compare::SizeOnly:
            return source.size != target.size ? QStringLiteral("size differs") : QString();
        case SyncOptions::Compare::Contents: {
            if (source.size != target.size)
                return QStringLiteral("size differs");
            const QString a = hashOf(sourceFs, source.uri, cancel);
            const QString b = hashOf(targetFs, target.uri, cancel);
            if (a.isEmpty() || b.isEmpty())
                return QStringLiteral("could not be compared");
            return a != b ? QStringLiteral("contents differ") : QString();
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
