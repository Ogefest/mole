#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QList>
#include <QStringList>

namespace mole {

/// Packs files and folders into a new archive, in the background.
///
/// Both ends go through IFileSystem, so packing a selection on a remote drive is
/// the same code as packing one on local disk. It writes a new archive and never
/// touches an existing one -- see docs/adr/0007-writing-archives.md.
class CompressTask final : public Task
{
    Q_OBJECT

public:
    /// What the archive is. Zip first because it is the one anyone can open
    /// anywhere; 7z is deliberately absent, because libarchive writes less of that
    /// format than it reads.
    enum class Format { Zip, TarGz, TarXz };

    /// The names as the interface offers them, in the order it offers them.
    static QStringList formatNames();
    static Format formatFromName(const QString& name);
    /// The suffix a chosen format wants, so a name can be completed for the user.
    static QString suffixFor(Format format);

    struct Request
    {
        FileSystemPtr sourceFileSystem;
        FileSystemPtr targetFileSystem;
        /// Files and folders to pack. Folders are packed with what is inside them.
        QList<VfsUri> sources;
        /// The archive to write. Must not already exist.
        VfsUri target;
        Format format = Format::Zip;
    };

    explicit CompressTask(Request request, QObject* parent = nullptr);

    int packedCount() const { return m_packed; }
    /// One human-readable line per entry that could not be read.
    const QStringList& failures() const { return m_failures; }

protected:
    void run() override;

private:
    /// Everything to write, in the order it will be written, with the name it will
    /// have inside the archive.
    struct Item
    {
        VfsUri source;
        QString archivePath;
        bool isDirectory = false;
        qint64 size = 0;
    };

    bool plan(QList<Item>& items);
    /// Removes a partly written archive. An archive that exists is one that
    /// finished; anything else would be mistaken for a good one.
    void discardPartialArchive();

    Request m_request;
    int m_packed = 0;
    QStringList m_failures;
};

} // namespace mole
