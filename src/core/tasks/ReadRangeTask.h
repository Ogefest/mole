#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Reads a window out of the middle of a file.
///
/// This is what makes previewing a 100 GB log possible: the viewer never holds
/// the file, only the slice it is showing. The backend is asked for a stream
/// and told to seek, so nothing before `offset` is transferred at all.
///
/// A backend without RandomAccessRead can still serve offset 0; asking for a
/// later window fails rather than quietly reading and discarding gigabytes.
class ReadRangeTask final : public Task
{
    Q_OBJECT

public:
    ReadRangeTask(
        FileSystemPtr fileSystem, VfsUri target, qint64 offset, qint64 length, QObject* parent = nullptr);

    /// Snaps the window to line boundaries, so a slice never opens or closes
    /// mid-line. Costs at most one extra line at each end.
    void setAlignToLines(bool align) { m_alignToLines = align; }

    /// Valid once finished() has been delivered.
    const QByteArray& contents() const { return m_contents; }
    /// Where the returned bytes actually start, which is not `offset` when the
    /// window was snapped to a line boundary.
    qint64 actualOffset() const { return m_actualOffset; }
    /// Size of the whole file, as reported by the backend. -1 when unknown.
    qint64 fileSize() const { return m_fileSize; }
    /// True when there are more bytes after this window.
    bool hasMore() const { return m_hasMore; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    qint64 m_offset = 0;
    qint64 m_length = 0;
    bool m_alignToLines = true;

    QByteArray m_contents;
    qint64 m_actualOffset = 0;
    qint64 m_fileSize = -1;
    bool m_hasMore = false;
};

} // namespace mole
