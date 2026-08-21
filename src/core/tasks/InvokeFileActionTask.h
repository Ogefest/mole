#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Does one of the things a drive said it could do.
///
/// Not background and not one of many, unlike the asking: this is a job somebody
/// picked out of a menu, it can take as long as signing a request takes, and if
/// it fails they have to be told which one failed.
class InvokeFileActionTask final : public Task
{
    Q_OBJECT

public:
    /// `title` is the drive's own name for the action, so what is on screen while
    /// it runs -- and in the failure if it fails -- is what was picked.
    InvokeFileActionTask(
        FileSystemPtr fileSystem, QString id, QString title, VfsUri target, QObject* parent = nullptr);

    const QString& actionId() const { return m_id; }
    const QString& actionTitle() const { return m_title; }
    const VfsUri& target() const { return m_target; }
    /// Only meaningful once this has succeeded.
    const FileActionOutcome& outcome() const { return m_outcome; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    QString m_id;
    QString m_title;
    VfsUri m_target;
    FileActionOutcome m_outcome;
};

} // namespace mole
