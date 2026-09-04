#pragma once

#include "sdk/IMetadataReader.h"

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// What is true of every file: how big it is, what it is, when it was touched.
///
/// The reader of last resort, and the only one that claims everything. It says
/// what `stat()` and the type sniff already know, which is exactly the nine
/// facts the information viewer used to build by hand -- built here instead, so
/// that a file with a viewer of its own gets them too.
class GenericMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.generic"); }
    /// Below everything: a reader that knows the format has more to say, and
    /// says it first.
    int priority() const override { return -1000; }
    bool canRead(const FileEntry& entry) const override { return !entry.isDir; }
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, const PluginServices& services,
        const CancelToken& cancel) const override;
};

/// Runs every reader that claimed a file and collects what they say.
///
/// On a worker thread, with the head read once and handed to all of them, and
/// the task's own cancel token passed down -- stepping to the next file has to
/// stop the reader for the last one, not wait for it.
///
/// A reader that fails costs its own rows and nothing else: it is caught here,
/// because one plugin's bad afternoon must not empty the panel.
class ReadMetadataTask final : public Task
{
public:
    ReadMetadataTask(FileSystemPtr fileSystem, FileEntry entry, QByteArray head,
        QList<IMetadataReader*> readers, PluginServices services, QObject* parent = nullptr);

    /// Valid once finished() has been delivered.
    const QList<FileFact>& facts() const { return m_facts; }
    /// Where each reader's contribution began in facts(), so the panel can put a
    /// line between one reader's answer and the next without inventing groups.
    const QList<int>& blockStarts() const { return m_blockStarts; }
    /// How many bytes of the file this task read for itself. Zero when the head
    /// it was given was enough, which is the usual case.
    qint64 bytesRead() const { return m_bytesRead; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    FileEntry m_entry;
    QByteArray m_head;
    QList<IMetadataReader*> m_readers;
    PluginServices m_services;

    QList<FileFact> m_facts;
    QList<int> m_blockStarts;
    qint64 m_bytesRead = 0;
};

} // namespace mole
