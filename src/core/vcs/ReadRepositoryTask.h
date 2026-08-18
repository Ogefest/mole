#pragma once

#include "core/tasks/Task.h"
#include "core/vcs/Repository.h"

namespace mole {

/// Asks git the questions that cost nothing once a repository is open: whether
/// this folder is inside a work tree, which branch it is on, and whether git is
/// part-way through something.
///
/// A task rather than a direct call for the reason every other read is one -- the
/// UI thread never waits on storage. Discovery walks up the tree looking for a
/// repository, and opening one reads its references; on a cold cache, or a
/// directory somebody has just plugged in, either can take long enough to be felt
/// as a window that stopped drawing.
///
/// Marked background, like QuerySpaceTask: nobody asked for it, and it must not
/// scroll the copy they did ask for off the task strip.
///
/// The work tree walk is not here. Per-file status costs a stat of the whole
/// checkout and is a task of its own, so the band can name the branch while the
/// counting is still going on.
class ReadRepositoryTask final : public Task
{
    Q_OBJECT

public:
    /// `localPath` is a real filesystem path -- libgit2 has no idea what a uri is,
    /// and this feature is local drives only.
    explicit ReadRepositoryTask(QString localPath, QObject* parent = nullptr);

    const QString& localPath() const { return m_localPath; }
    /// The work tree root, or empty when the path is in no repository. Only
    /// meaningful once the task has succeeded.
    const QString& root() const { return m_root; }
    /// The repository's own directory, for whoever has to notice a commit made
    /// outside Mole. Empty when the path is in no repository.
    const QString& gitDir() const { return m_gitDir; }
    const RepositoryHead& head() const { return m_head; }

signals:
    /// Emitted on the UI thread, and emitted even when there is no repository:
    /// `root` empty is an answer rather than a failure, and it is the answer that
    /// makes the band go away when somebody navigates out of a checkout.
    void repositoryRead(const QString& localPath, const QString& root, mole::RepositoryHead head);

protected:
    void run() override;

private:
    QString m_localPath;
    QString m_root;
    QString m_gitDir;
    RepositoryHead m_head;
};

} // namespace mole
