#pragma once

#include "core/index/IndexDatabase.h"
#include "core/index/ScanOptions.h"
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

    /// What this scan was asked for, as one thing rather than three calls a
    /// caller can make two of.
    ///
    /// `incremental` keeps what has not changed rather than rewriting it. A
    /// directory whose modification time has not moved since the last scan has
    /// the same children, so its subtree is carried across and not walked. On
    /// the trees this exists for that is the difference between minutes and
    /// hours to learn that nothing much has moved.
    ///
    /// **Nothing is carried forward that this walk did not just see in a
    /// listing.** That is what makes it correct rather than only fast: a
    /// directory that has been deleted is not in its parent's listing, so it is
    /// not carried, so it goes -- whatever its parent's timestamp says.
    ///
    /// A full rescan is this turned off, and is what somebody reaches for when
    /// they suspect the index.
    ///
    /// `metadata` and `archives` say what the scan was asked to record; the
    /// readers that do it are installed separately, because core cannot name
    /// them. `mole::applyScanOptions()` in `sdk` does both in one line, which is
    /// what every caller with services to hand should use.
    void setOptions(const ScanOptions& options) { m_options = options; }
    const ScanOptions& options() const { return m_options; }

    /// How many entries came across untouched, and whether the drive gave the
    /// scan anything to go on.
    qint64 carriedForward() const { return m_carried; }
    /// True when the drive dates none of its folders, so the scan could not
    /// skip anything and walked the lot. Said out loud rather than left to look
    /// like a slow incremental scan.
    bool datesFolders() const { return m_datesFolders; }

    /// Rows for what lives inside a file that is a container.
    ///
    /// A zip of years-old projects holds a great deal of what somebody is
    /// looking for, and none of it could be found by any means at all. Core has
    /// no idea what an archive is -- the backend that mounts one is a plugin --
    /// so the rows come from whoever does, the same way the facts do.
    ///
    /// Returning nothing is normal: an ordinary file, a container nothing can
    /// open, a corrupt one, or one that needs a password. None of those is a
    /// reason for the scan to stop.
    /// `truncatedOut` is set when the container held more than the reader was
    /// willing to take, so the container's own row can say it was cut rather
    /// than being trimmed in silence.
    void setContainerReader(std::function<QList<IndexedFile>(const FileEntry&, bool*)> reader);

    /// How many entries a scan took out of containers, and how many containers
    /// gave up more than the ceiling allows.
    qint64 containedEntries() const { return m_containedEntries; }

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
    qint64 m_containedEntries = 0;
    ScanOptions m_options;
    bool m_datesFolders = false;
    qint64 m_carried = 0;
    std::function<QList<IndexedFile>(const FileEntry&, bool*)> m_containers;
    qint64 m_skippedDirectories = 0;
    std::function<QList<SearchFact>(const FileEntry&)> m_facts;
};

} // namespace mole
