#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Pulls (part of) a file's bytes off any backend, off the UI thread.
///
/// Previews use this so that opening a 2 GB log over SFTP reads the first few
/// kilobytes instead of the whole thing.
class ReadFileTask final : public Task
{
    Q_OBJECT

public:
    /// `maxBytes` of -1 reads the whole file.
    ReadFileTask(FileSystemPtr fileSystem, VfsUri target, qint64 maxBytes = -1, QObject* parent = nullptr);

    /// Also write what was read to this local path, on the task's own thread.
    ///
    /// The read was moved onto a worker "so that opening a 200 MB file out of a
    /// zip does not stall the window", and then both callers wrote the same
    /// bytes back out in the finished handler -- on the thread that draws. A
    /// write of 200 MB to /tmp, or to a MOLE_STAGING_DIR on a network share, is
    /// a stall of the same order the task was avoiding; and neither of them
    /// looked at whether it worked. Asked for here, the write happens where the
    /// read did and its failure is the task's failure. See MOLE-406.
    ///
    /// Call before submitting. An empty path writes nothing, which is what a
    /// caller that only wants the bytes gets.
    void landAt(QString path) { m_landAt = std::move(path); }
    /// Where the bytes were written, once the task has succeeded.
    const QString& landedAt() const { return m_landAt; }

    /// Valid once finished() has been delivered.
    const QByteArray& contents() const { return m_contents; }
    /// True when the file was longer than maxBytes.
    bool truncated() const { return m_truncated; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    qint64 m_maxBytes = -1;
    QString m_landAt;
    QByteArray m_contents;
    bool m_truncated = false;
};

} // namespace mole
