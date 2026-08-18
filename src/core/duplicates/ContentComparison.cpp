#include "core/duplicates/ContentComparison.h"

#include <QIODevice>

#include <memory>
#include <vector>

namespace mole {
namespace {

    /// One file being read alongside the others it is being compared with.
    struct Reader
    {
        FileEntry entry;
        std::unique_ptr<QIODevice> device;
        QByteArray chunk;

        /// Stops reading this one and gives back its chunk and its descriptor.
        /// Called the moment a file is alone in its class: nothing else is going
        /// to be compared against it, so holding it open is holding memory for
        /// no answer.
        void release()
        {
            device.reset();
            chunk.clear();
            chunk.squeeze();
        }
    };

    /// Reads exactly `want` bytes, or what is left of the file.
    ///
    /// Not one call to read(): a network stream is sequential and hands over
    /// whatever has arrived, so two identical files read at the same offset can
    /// come back with different amounts. A hash does not care -- it sees the same
    /// bytes whatever the chunking -- but a comparison that lined up a short read
    /// against a full one would call two identical files different.
    ///
    /// A stream that stops answering ends the read short, which separates that
    /// file from the others and drops it. That is the safe direction: a duplicate
    /// missed, rather than a file wrongly declared a copy of something else.
    QByteArray readChunk(QIODevice& device, qint64 want)
    {
        QByteArray out;
        out.reserve(want);
        while (out.size() < want) {
            const QByteArray part = device.read(want - out.size());
            if (!part.isEmpty()) {
                out += part;
                continue;
            }
            if (device.atEnd() || !device.isSequential())
                break;
            if (!device.waitForReadyRead(30000))
                break;
        }
        return out;
    }

    /// Splits one class by the chunk each of its files just read.
    ///
    /// Against the first file of each part rather than through a hash table: the
    /// parts are almost always one, because a class that has agreed this far
    /// usually agrees to the end, so this is one memcmp per file. Hashing a
    /// quarter-megabyte chunk to key a table would cost more than the comparison
    /// it was standing in for.
    QList<QList<int>> splitByChunk(const QList<int>& members, const std::vector<Reader>& readers)
    {
        QList<QList<int>> parts;
        for (int index : members) {
            bool placed = false;
            for (QList<int>& part : parts) {
                if (readers[index].chunk == readers[part.first()].chunk) {
                    part.append(index);
                    placed = true;
                    break;
                }
            }
            if (!placed)
                parts.append(QList<int> { index });
        }
        return parts;
    }

    QList<FileEntry> entriesOf(const QList<int>& members, const std::vector<Reader>& readers)
    {
        QList<FileEntry> out;
        out.reserve(members.size());
        for (int index : members)
            out.append(readers[index].entry);
        return out;
    }

    /// Compares files that can all be held open at once.
    QList<QList<FileEntry>> compareTogether(
        const QList<FileEntry>& files, const DriveLookup& driveFor, const CancelToken& cancel)
    {
        std::vector<Reader> readers;
        readers.reserve(files.size());
        for (const FileEntry& entry : files) {
            IFileSystem* drive = driveFor(entry);
            if (!drive)
                continue;
            Result<std::unique_ptr<QIODevice>> opened = drive->openRead(entry.uri);
            if (!opened.ok() || !opened.value())
                continue; // unreadable, and not a match for everything else that is
            readers.push_back(Reader { entry, std::move(opened.value()), {} });
        }

        // Classes as indices into `readers`, refined a chunk at a time. `live`
        // holds the ones with something still to prove; a class that falls to one
        // file has nothing left to compare against and is set aside.
        QList<QList<int>> live;
        QList<QList<int>> alone;
        if (readers.size() > 1) {
            QList<int> everything;
            for (int i = 0; i < static_cast<int>(readers.size()); ++i)
                everything.append(i);
            live.append(everything);
        } else {
            for (int i = 0; i < static_cast<int>(readers.size()); ++i)
                alone.append(QList<int> { i });
        }

        while (!live.isEmpty()) {
            if (cancel.isCancelled())
                return {};

            bool anythingRead = false;
            for (const QList<int>& members : live) {
                for (int index : members) {
                    readers[index].chunk = readChunk(*readers[index].device, kComparisonChunkBytes);
                    anythingRead = anythingRead || !readers[index].chunk.isEmpty();
                }
            }
            // Every file still in play is at its end, and they have agreed the
            // whole way: the classes that are left are the answer.
            if (!anythingRead)
                break;

            QList<QList<int>> next;
            for (const QList<int>& members : live) {
                for (const QList<int>& part : splitByChunk(members, readers)) {
                    if (part.size() > 1) {
                        next.append(part);
                    } else {
                        alone.append(part);
                        readers[part.first()].release();
                    }
                }
            }
            live = next;
        }

        QList<QList<FileEntry>> out;
        out.reserve(live.size() + alone.size());
        for (const QList<int>& members : live)
            out.append(entriesOf(members, readers));
        for (const QList<int>& members : alone)
            out.append(entriesOf(members, readers));
        return out;
    }

    /// Whether two files are identical, used to join classes found in different
    /// slices of a bucket too large to open at once.
    bool sameContents(
        const FileEntry& a, const FileEntry& b, const DriveLookup& driveFor, const CancelToken& cancel)
    {
        return compareTogether({ a, b }, driveFor, cancel).size() == 1;
    }

} // namespace

QList<QList<FileEntry>> partitionByContents(
    const QList<FileEntry>& files, const DriveLookup& driveFor, const CancelToken& cancel)
{
    if (files.size() < 2)
        return files.isEmpty() ? QList<QList<FileEntry>> {} : QList<QList<FileEntry>> { files };

    if (files.size() <= kMaxOpenAtOnce)
        return compareTogether(files, driveFor, cancel);

    // More files than may be held open at once -- thousands of copies of one
    // photograph, which is a real shape and not a contrived one. Compared a slice
    // at a time, and the classes each slice found are then joined by comparing
    // one file from each.
    //
    // Slices keep their lone files rather than discarding them, because a file
    // alone in its slice is very often a match for a class in another one.
    QList<QList<FileEntry>> found;
    for (qsizetype at = 0; at < files.size(); at += kMaxOpenAtOnce) {
        if (cancel.isCancelled())
            return {};
        found.append(compareTogether(files.mid(at, kMaxOpenAtOnce), driveFor, cancel));
    }

    QList<QList<FileEntry>> merged;
    for (const QList<FileEntry>& candidate : std::as_const(found)) {
        if (candidate.isEmpty())
            continue;
        bool joined = false;
        for (QList<FileEntry>& existing : merged) {
            if (cancel.isCancelled())
                return {};
            // One file from each side, because every file within a class has
            // already been proved identical to the one it is represented by.
            if (sameContents(existing.first(), candidate.first(), driveFor, cancel)) {
                existing.append(candidate);
                joined = true;
                break;
            }
        }
        if (!joined)
            merged.append(candidate);
    }
    return merged;
}

} // namespace mole
