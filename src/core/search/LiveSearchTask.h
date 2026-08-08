#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Searches the filesystem as it is right now by walking it, as opposed to
/// IndexDatabase::search() which answers from a previous scan. Slower but
/// always current -- the two are deliberately separate features.
class LiveSearchTask final : public Task
{
    Q_OBJECT

public:
    struct Criteria
    {
        QString text;
        QString extension;
        bool caseSensitive = false;
        bool includeDirs = true;
        bool includeFiles = true;
        qint64 minSize = -1;
        qint64 maxSize = -1;
        int maxResults = 10000;
    };

    LiveSearchTask(FileSystemPtr fileSystem, VfsUri root, Criteria criteria, QObject* parent = nullptr);

    qint64 hitCount() const { return m_hitCount; }
    bool truncated() const { return m_truncated; }

signals:
    /// Emitted in batches on the UI thread while the walk is still running, so
    /// results stream in instead of appearing all at once at the end.
    void hitsFound(const mole::FileEntryList& batch);

protected:
    void run() override;

private:
    static constexpr int kEmitBatchSize = 200;

    bool matches(const FileEntry& entry) const;

    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    Criteria m_criteria;
    QString m_foldedText;
    qint64 m_hitCount = 0;
    bool m_truncated = false;
};

} // namespace mole
