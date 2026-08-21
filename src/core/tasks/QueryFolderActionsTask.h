#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive which entries in one folder it has something for.
///
/// One query for the folder, never one per row: a folder of five thousand files
/// must not become five thousand lookups on the path that draws. That is the
/// whole reason this exists rather than the listing asking about each entry as
/// it goes -- see IFileSystem::entriesWithActions().
///
/// Background and one of many, like the listing it follows, and it holds nothing
/// up: the rows are drawn when the listing lands and the marks appear when this
/// does. The same shape the git band already has.
class QueryFolderActionsTask final : public Task
{
    Q_OBJECT

public:
    QueryFolderActionsTask(FileSystemPtr fileSystem, VfsUri directory, QObject* parent = nullptr);

    const VfsUri& directory() const { return m_directory; }
    /// The names of the entries the drive has something for.
    const QStringList& names() const { return m_names; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_directory;
    QStringList m_names;
};

} // namespace mole
