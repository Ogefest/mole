#include "plugins/rclone/RcloneFileSystem.h"

#include "plugins/rclone/RcloneLibrary.h"

#include <QBuffer>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryFile>

namespace mole {
namespace {

    /// A file on disk that removes itself when the handle goes away.
    ///
    /// QTemporaryFile deletes on destruction, but its handle cannot be reopened for
    /// reading without keeping the object alive alongside -- which is how the first
    /// version of this leaked both the object and the file.
    class ScratchFile : public QFile
    {
    public:
        explicit ScratchFile(const QString& path)
            : QFile(path)
        {
        }

        ~ScratchFile() override
        {
            QFile::close();
            if (!fileName().isEmpty())
                QFile::remove(fileName());
        }
    };

    /// A remote file being written, which is copied back when it closes.
    ///
    /// rclone's remote-control interface has no streaming write, so a write goes to
    /// a scratch file and is uploaded on close. That is a real limitation and worth
    /// naming: writing a file larger than the local disk will not work, and the
    /// upload happens at close rather than as bytes arrive.
    class UploadOnClose final : public ScratchFile
    {
    public:
        UploadOnClose(const QString& path, QString connectionString, QString remotePath)
            : ScratchFile(path)
            , m_connectionString(std::move(connectionString))
            , m_remotePath(std::move(remotePath))
        {
        }

        ~UploadOnClose() override { flushToRemote(); }

        void close() override
        {
            QFile::flush();
            flushToRemote();
            ScratchFile::close();
        }

    private:
        void flushToRemote()
        {
            if (m_uploaded || fileName().isEmpty())
                return;
            m_uploaded = true;

            QFile::flush();

            const int slash = m_remotePath.lastIndexOf(QLatin1Char('/'));
            const QString directory = slash > 0 ? m_remotePath.left(slash) : QString();
            const QString name = slash >= 0 ? m_remotePath.mid(slash + 1) : m_remotePath;

            const QFileInfo local(fileName());
            QJsonObject input {
                { QStringLiteral("srcFs"), local.absolutePath() },
                { QStringLiteral("srcRemote"), local.fileName() },
                { QStringLiteral("dstFs"),
                    m_connectionString + (directory.isEmpty() ? QString() : QLatin1Char('/') + directory) },
                { QStringLiteral("dstRemote"), name },
            };
            RcloneLibrary::instance().call(QStringLiteral("operations/copyfile"), input);
        }

        QString m_connectionString;
        QString m_remotePath;
        bool m_uploaded = false;
    };

} // namespace

RcloneFileSystem::RcloneFileSystem(QString scheme, QString connectionString, QString root)
    : m_scheme(std::move(scheme))
    , m_connectionString(std::move(connectionString))
    , m_root(std::move(root))
{
    // Trailing slashes would turn "root/" + "path" into a double separator,
    // which some backends take literally.
    while (m_root.endsWith(QLatin1Char('/')))
        m_root.chop(1);
}

VfsCapabilities RcloneFileSystem::capabilities() const
{
    // No RandomAccessRead: rclone can serve a whole object, not a seek into one,
    // so the paging text viewer falls back to reading from the start rather
    // than pretending it can jump.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::ReportsSpace;
}

VfsError RcloneFileSystem::errorFrom(const QString& message)
{
    // rclone's wording, mapped to the codes this application acts on. Anything
    // it does not recognise stays as the message it came with.
    //
    // Order matters: "not a directory" also contains "not", and several of
    // these overlap. The most specific reading wins.
    if (message.contains(QStringLiteral("not a directory"), Qt::CaseInsensitive))
        return VfsError::make(VfsError::NotADirectory, message);
    if (message.contains(QStringLiteral("is a directory"), Qt::CaseInsensitive))
        return VfsError::make(VfsError::IsADirectory, message);
    if (message.contains(QStringLiteral("already exists"), Qt::CaseInsensitive))
        return VfsError::make(VfsError::AlreadyExists, message);
    if (message.contains(QStringLiteral("not found"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("does not exist"), Qt::CaseInsensitive)) {
        return VfsError::make(VfsError::NotFound, message);
    }
    if (message.contains(QStringLiteral("permission"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("denied"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("401")) || message.contains(QStringLiteral("403"))) {
        return VfsError::make(VfsError::AccessDenied, message);
    }
    if (message.contains(QStringLiteral("directory not empty"), Qt::CaseInsensitive))
        return VfsError::make(VfsError::NotEmpty, message);
    return VfsError::make(VfsError::NetworkError, message);
}

QString RcloneFileSystem::remotePathFor(const VfsUri& uri) const
{
    // Relative to the mount, because the mount root is part of `fs`.
    QString path = uri.path();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    return path;
}

FileEntry RcloneFileSystem::entryFromJson(const QJsonObject& json, const VfsUri& parent) const
{
    FileEntry entry;
    entry.name = json.value(QStringLiteral("Name")).toString();
    entry.isDir = json.value(QStringLiteral("IsDir")).toBool();
    entry.size = entry.isDir ? 0 : static_cast<qint64>(json.value(QStringLiteral("Size")).toDouble());
    entry.modified
        = QDateTime::fromString(json.value(QStringLiteral("ModTime")).toString(), Qt::ISODateWithMs);
    entry.mimeType = json.value(QStringLiteral("MimeType")).toString();
    entry.uri = parent.child(entry.name);
    // Remote objects have no meaningful local permissions, and inventing some
    // would be worse than saying nothing: the access tag stays empty.
    entry.isReadable = true;
    entry.isWritable = true;
    return entry;
}

Result<FileEntryList> RcloneFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    // Checked before the call, not during it. rclone's remote-control interface
    // runs this synchronously with no handle to interrupt, so a request already
    // in flight finishes -- but a navigation the user has already moved on from
    // must not start a network round trip at all, which is the case that
    // actually costs something.
    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Cancelled"));

    QString error;
    const QJsonObject reply = RcloneLibrary::instance().call(QStringLiteral("operations/list"),
        QJsonObject { { QStringLiteral("fs"), fsSpec() }, { QStringLiteral("remote"), remotePathFor(dir) } },
        &error);

    if (!error.isEmpty())
        return errorFrom(error);

    FileEntryList entries;
    const QJsonArray list = reply.value(QStringLiteral("list")).toArray();
    entries.reserve(list.size());
    for (const QJsonValue& value : list)
        entries.append(entryFromJson(value.toObject(), dir));
    return entries;
}

Result<FileEntry> RcloneFileSystem::stat(const VfsUri& target)
{
    // rclone has no stat, so the parent is listed and the entry picked out.
    // Costly on a large directory, and the alternative -- a HEAD per backend --
    // is not something the remote-control interface offers.
    const VfsUri parent = target.parent();
    Result<FileEntryList> listed = list(parent, CancelToken());
    if (!listed.ok())
        return listed.error();

    for (const FileEntry& entry : listed.value()) {
        if (entry.name == target.fileName())
            return entry;
    }
    return VfsError::make(VfsError::NotFound, QStringLiteral("No such file: %1").arg(target.toString()));
}

Result<SpaceInfo> RcloneFileSystem::space(const VfsUri& target)
{
    Q_UNUSED(target)

    QString error;
    const QJsonObject reply = RcloneLibrary::instance().call(
        QStringLiteral("operations/about"), QJsonObject { { QStringLiteral("fs"), fsSpec() } }, &error);

    if (!error.isEmpty()) {
        // Plenty of remotes cannot say, which is a normal answer rather than a
        // failure: the sidebar then shows a name and no bar.
        return VfsError::make(VfsError::NotSupported, error);
    }

    SpaceInfo info;
    info.totalBytes = static_cast<qint64>(reply.value(QStringLiteral("total")).toDouble());
    info.freeBytes = static_cast<qint64>(reply.value(QStringLiteral("free")).toDouble());
    if (info.totalBytes <= 0)
        return VfsError::make(VfsError::NotSupported, QStringLiteral("This remote has no size"));
    return info;
}

Result<void> RcloneFileSystem::makeDirectory(const VfsUri& target)
{
    // rclone's mkdir is idempotent: it succeeds on a directory that is already
    // there. Every other backend here refuses, and it has to -- "create folder"
    // that quietly merges into an existing one is how work gets mixed together
    // without anybody being told. One extra round trip is the price.
    if (Result<FileEntry> existing = stat(target); existing.ok()) {
        return VfsError::make(
            VfsError::AlreadyExists, QStringLiteral("Already there: %1").arg(target.toString()));
    }

    QString error;
    RcloneLibrary::instance().call(QStringLiteral("operations/mkdir"),
        QJsonObject {
            { QStringLiteral("fs"), fsSpec() }, { QStringLiteral("remote"), remotePathFor(target) } },
        &error);
    if (!error.isEmpty())
        return errorFrom(error);
    return {};
}

Result<void> RcloneFileSystem::remove(const VfsUri& target, bool recursive)
{
    Result<FileEntry> info = stat(target);
    const bool isDirectory = info.ok() && info.value().isDir;

    QString error;
    if (isDirectory) {
        // purge removes a tree, rmdir refuses a non-empty one -- which is the
        // difference the caller asked for.
        RcloneLibrary::instance().call(
            recursive ? QStringLiteral("operations/purge") : QStringLiteral("operations/rmdir"),
            QJsonObject {
                { QStringLiteral("fs"), fsSpec() }, { QStringLiteral("remote"), remotePathFor(target) } },
            &error);
    } else {
        RcloneLibrary::instance().call(QStringLiteral("operations/deletefile"),
            QJsonObject {
                { QStringLiteral("fs"), fsSpec() }, { QStringLiteral("remote"), remotePathFor(target) } },
            &error);
    }

    if (!error.isEmpty())
        return errorFrom(error);
    return {};
}

Result<void> RcloneFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    // Same reason as mkdir: rclone's move overwrites, and every other backend
    // here refuses. A rename that silently replaces the file already at the
    // destination destroys it with no warning at all.
    if (Result<FileEntry> existing = stat(to); existing.ok()) {
        return VfsError::make(
            VfsError::AlreadyExists, QStringLiteral("Already there: %1").arg(to.toString()));
    }

    QString error;
    RcloneLibrary::instance().call(QStringLiteral("operations/movefile"),
        QJsonObject { { QStringLiteral("srcFs"), fsSpec() },
            { QStringLiteral("srcRemote"), remotePathFor(from) }, { QStringLiteral("dstFs"), fsSpec() },
            { QStringLiteral("dstRemote"), remotePathFor(to) } },
        &error);
    if (!error.isEmpty())
        return errorFrom(error);
    return {};
}

Result<std::unique_ptr<QIODevice>> RcloneFileSystem::openRead(const VfsUri& target)
{
    // Fetched whole into a scratch file. The remote-control interface serves
    // objects, not ranges, so there is no streaming read to offer -- which is
    // why RandomAccessRead is not advertised and the paging viewer knows not to
    // ask.
    QString scratchPath;
    {
        QTemporaryFile reserved;
        if (!reserved.open())
            return VfsError::make(VfsError::IoError, QStringLiteral("No scratch space"));
        reserved.setAutoRemove(false); // ScratchFile owns it from here
        scratchPath = reserved.fileName();
    }

    const QString remote = remotePathFor(target);
    const int slash = remote.lastIndexOf(QLatin1Char('/'));
    const QString directory = slash > 0 ? remote.left(slash) : QString();
    const QString name = slash >= 0 ? remote.mid(slash + 1) : remote;

    const QFileInfo local(scratchPath);
    QString error;
    RcloneLibrary::instance().call(QStringLiteral("operations/copyfile"),
        QJsonObject { { QStringLiteral("srcFs"),
                          fsSpec() + (directory.isEmpty() ? QString() : QLatin1Char('/') + directory) },
            { QStringLiteral("srcRemote"), name }, { QStringLiteral("dstFs"), local.absolutePath() },
            { QStringLiteral("dstRemote"), local.fileName() } },
        &error);

    if (!error.isEmpty())
        return errorFrom(error);

    auto handle = std::make_unique<ScratchFile>(scratchPath);
    if (!handle->open(QIODevice::ReadOnly))
        return VfsError::make(VfsError::IoError, QStringLiteral("Could not read what arrived"));
    return std::unique_ptr<QIODevice>(handle.release());
}

Result<std::unique_ptr<QIODevice>> RcloneFileSystem::openWrite(const VfsUri& target)
{
    QString path;
    {
        QTemporaryFile reserved;
        if (!reserved.open())
            return VfsError::make(VfsError::IoError, QStringLiteral("No scratch space"));
        reserved.setAutoRemove(false); // UploadOnClose owns it from here
        path = reserved.fileName();
    }

    auto upload = std::make_unique<UploadOnClose>(path, fsSpec(), remotePathFor(target));
    if (!upload->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return VfsError::make(VfsError::IoError, QStringLiteral("No scratch space"));
    return std::unique_ptr<QIODevice>(upload.release());
}

} // namespace mole
