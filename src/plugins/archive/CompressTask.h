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
    /// anywhere. `Xz` is a bare xz stream with no container, which is why it holds
    /// exactly one file and nothing else -- see takesOneFileOnly().
    enum class Format { Zip, TarGz, TarXz, SevenZip, Xz };

    /// The names as the interface offers them, in the order it offers them.
    static QStringList formatNames();
    static Format formatFromName(const QString& name);
    /// The suffix a chosen format wants, so a name can be completed for the user.
    static QString suffixFor(Format format);
    /// Only zip carries a password: a tar has no notion of one and gzip and xz
    /// encrypt nothing. Asked rather than assumed, so the interface never offers a
    /// box that would be quietly ignored.
    static bool formatSupportsPassword(Format format);
    /// True for a format with no container to put a second thing in: a bare `.xz`
    /// is one compressed stream, so it can carry one file and not a folder. Asked
    /// rather than discovered halfway through writing.
    static bool takesOneFileOnly(Format format);
    /// The same name with the suffix this format wants. Keeps the base -- including
    /// any dots in it -- because changing the kind must not throw away a name
    /// somebody typed.
    static QString nameWithSuffix(const QString& name, Format format);

    struct Request
    {
        FileSystemPtr sourceFileSystem;
        FileSystemPtr targetFileSystem;
        /// Files and folders to pack. Folders are packed with what is inside them.
        QList<VfsUri> sources;
        /// The archive to write. Must not already exist.
        VfsUri target;
        Format format = Format::Zip;
        /// Empty for an archive anyone can open. Set, the contents are encrypted --
        /// AES-256 for zip, and refused outright for a format that cannot carry it,
        /// because writing something unencrypted when a password was asked for is
        /// the worst possible answer.
        QString passphrase;
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
