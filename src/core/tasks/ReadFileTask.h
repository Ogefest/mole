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
    QByteArray m_contents;
    bool m_truncated = false;
};

} // namespace mole
