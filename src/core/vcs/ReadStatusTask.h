#pragma once

#include "core/tasks/Task.h"
#include "core/vcs/Repository.h"

namespace mole {

/// Walks a work tree and answers what has changed in it.
///
/// The expensive half of knowing about a checkout, and the reason it is a task of
/// its own rather than part of ReadRepositoryTask: the branch is a handful of
/// reference reads and should appear at once, while this stats every file git is
/// tracking. Splitting them is what lets the band name the branch while the
/// counting is still going on.
///
/// Marked background, like QuerySpaceTask: nobody asked for it, and it must not
/// scroll the copy they did ask for off the task strip.
///
/// One walk per work tree, not per folder. The answer goes into
/// RepositoryStatusCache, and a task that finds one already there returns it
/// rather than walking again -- so navigating from `src/` to `tests/` inside one
/// checkout costs nothing, and two panes on one checkout share a single walk.
class ReadStatusTask final : public Task
{
    Q_OBJECT

public:
    /// `localPath` is any real filesystem path inside the work tree; the walk
    /// covers the whole of it either way, because that is what git status is.
    explicit ReadStatusTask(QString localPath, QObject* parent = nullptr);

    const QString& localPath() const { return m_localPath; }
    /// The work tree root, or empty when the path is in no repository.
    const QString& root() const { return m_root; }
    const RepositoryStatus& status() const { return m_status; }
    /// Whether the answer came out of the cache rather than off the disk. Tests
    /// assert on this; nothing in the application needs to know.
    bool wasCached() const { return m_cached; }

signals:
    /// Emitted on the UI thread, and only with an answer worth showing: a walk
    /// that was cancelled part way emits nothing, because a partial status is a
    /// listing marked wrong rather than a listing marked less.
    void statusRead(const QString& root, mole::RepositoryStatus status);

protected:
    void run() override;

private:
    QString m_localPath;
    QString m_root;
    RepositoryStatus m_status;
    bool m_cached = false;
};

} // namespace mole
