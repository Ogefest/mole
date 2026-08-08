#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

namespace mole {
namespace {

    /// "rwxr-xr--", the form everyone reads permissions in. Built from Qt's
    /// owner/group/other flags rather than a raw octal number so a change is
    /// legible in an alert without decoding anything.
    QString permissionString(QFile::Permissions permissions)
    {
        static constexpr QFile::Permission kBits[9]
            = { QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner, QFile::ReadGroup, QFile::WriteGroup,
                  QFile::ExeGroup, QFile::ReadOther, QFile::WriteOther, QFile::ExeOther };
        static constexpr char kLetters[9] = { 'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x' };

        QString out;
        out.reserve(9);
        for (int i = 0; i < 9; ++i)
            out.append(permissions.testFlag(kBits[i]) ? QLatin1Char(kLetters[i]) : QLatin1Char('-'));
        return out;
    }

    FileEntry entryFromInfo(const QFileInfo& info, const VfsUri& uri)
    {
        FileEntry e;
        e.name = info.fileName();
        e.uri = uri;
        e.isDir = info.isDir();
        e.isSymlink = info.isSymLink();
        e.isHidden = info.isHidden();
        e.isReadable = info.isReadable();
        e.isWritable = info.isWritable();
        e.size = info.isDir() ? 0 : info.size();
        e.modified = info.lastModified();
        e.permissions = permissionString(info.permissions());
        return e;
    }

    VfsError errorForPath(const QString& path)
    {
        const QFileInfo info(path);
        if (!info.exists())
            return VfsError::make(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));
        if (!info.isReadable())
            return VfsError::make(VfsError::AccessDenied, QStringLiteral("Access denied: %1").arg(path));
        return VfsError::make(VfsError::IoError, QStringLiteral("I/O error on %1").arg(path));
    }

} // namespace

VfsCapabilities LocalFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead
        | VfsCapability::ReportsSpace | VfsCapability::ReportsAccess;
}

Result<SpaceInfo> LocalFileSystem::space(const VfsUri& target)
{
    // QStorageInfo hits the filesystem, which is why this is behind the task
    // layer: on a hung network mount the call blocks, and blocking here would
    // freeze the window if anyone were tempted to ask from the UI thread.
    const QStorageInfo storage(target.path());
    if (!storage.isValid() || !storage.isReady()) {
        return VfsError::make(
            VfsError::NotFound, QStringLiteral("No volume is mounted at %1").arg(target.path()));
    }

    const qint64 total = storage.bytesTotal();
    if (total <= 0) {
        // Pseudo filesystems report zero. Saying nothing beats charting a
        // drive as 0%% full when it has no size to speak of.
        return VfsError::make(VfsError::NotSupported, QStringLiteral("This volume has no size"));
    }

    SpaceInfo info;
    info.totalBytes = total;
    info.freeBytes = std::max<qint64>(0, storage.bytesAvailable());
    return info;
}

Result<AccessInfo> LocalFileSystem::access(const VfsUri& target)
{
    const QFileInfo info(target.path());
    if (!info.exists())
        return errorForPath(target.path());

    const auto answer = [](bool yes) { return yes ? AccessInfo::Answer::Yes : AccessInfo::Answer::No; };

    AccessInfo out;
    // isReadable/isWritable answer for *this* process, which is the question
    // that actually matters, and they answer it on Windows too -- Qt consults
    // the ACL there rather than pretending there are mode bits.
    out.read = answer(info.isReadable());
    out.write = answer(info.isWritable());
    out.createInside = info.isDir() ? answer(info.isWritable()) : AccessInfo::Answer::No;

    // Removing an entry is governed by the *parent* directory, not by the entry
    // itself -- a read-only file in a writable folder can still be deleted.
    const QFileInfo parent(info.absolutePath());
    out.remove = answer(parent.isWritable());

    // Only the owner (or root) may change a file's permissions. Qt has no
    // portable question for it, so it is left unknown rather than guessed.
    out.owner = info.owner();
    out.group = info.group();

    // The mode string is the platform's own form and stays platform-specific:
    // on Windows QFileInfo::permissions() synthesises something from the ACL
    // that would be misleading written out as nine characters, so it is only
    // offered where it means what it says.
#ifdef Q_OS_WIN
    out.nativeText.clear();
#else
    out.nativeText = permissionString(info.permissions());
#endif

    return out;
}

Result<FileEntryList> LocalFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    const QString path = dir.toLocalPath();
    if (path.isEmpty())
        return VfsError::make(VfsError::NotSupported, QStringLiteral("Not a local uri"));

    const QFileInfo dirInfo(path);
    if (!dirInfo.exists())
        return errorForPath(path);
    if (!dirInfo.isDir())
        return VfsError::make(VfsError::NotADirectory, QStringLiteral("Not a directory: %1").arg(path));

    FileEntryList out;
    QDirIterator it(
        path, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        if (cancel.isCancelled())
            return VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled"));

        it.next();
        const QFileInfo info = it.fileInfo();
        out.append(entryFromInfo(info, dir.child(info.fileName())));
    }

    return out;
}

Result<FileEntry> LocalFileSystem::stat(const VfsUri& target)
{
    const QString path = target.toLocalPath();
    if (path.isEmpty())
        return VfsError::make(VfsError::NotSupported, QStringLiteral("Not a local uri"));

    const QFileInfo info(path);
    if (!info.exists())
        return errorForPath(path);

    return entryFromInfo(info, target);
}

Result<void> LocalFileSystem::makeDirectory(const VfsUri& target)
{
    const QString path = target.toLocalPath();
    if (path.isEmpty())
        return Result<void>::failure(VfsError::NotSupported, QStringLiteral("Not a local uri"));
    if (QFileInfo::exists(path))
        return Result<void>::failure(VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(path));
    if (!QDir().mkpath(path))
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Cannot create %1").arg(path));
    return {};
}

Result<void> LocalFileSystem::remove(const VfsUri& target, bool recursive)
{
    const QString path = target.toLocalPath();
    if (path.isEmpty())
        return Result<void>::failure(VfsError::NotSupported, QStringLiteral("Not a local uri"));

    const QFileInfo info(path);
    if (!info.exists())
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));

    if (info.isDir()) {
        QDir dir(path);
        if (recursive) {
            if (!dir.removeRecursively())
                return Result<void>::failure(
                    VfsError::IoError, QStringLiteral("Cannot remove directory %1").arg(path));
        } else if (!dir.rmdir(path)) {
            return Result<void>::failure(
                VfsError::NotEmpty, QStringLiteral("Directory not empty: %1").arg(path));
        }
        return {};
    }

    if (!QFile::remove(path))
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Cannot remove %1").arg(path));
    return {};
}

Result<void> LocalFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const QString src = from.toLocalPath();
    const QString dst = to.toLocalPath();
    if (src.isEmpty() || dst.isEmpty())
        return Result<void>::failure(VfsError::NotSupported, QStringLiteral("Not a local uri"));
    if (QFileInfo::exists(dst))
        return Result<void>::failure(VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(dst));
    if (!QFile::rename(src, dst))
        return Result<void>::failure(
            VfsError::IoError, QStringLiteral("Cannot rename %1 to %2").arg(src, dst));
    return {};
}

Result<std::unique_ptr<QIODevice>> LocalFileSystem::openRead(const VfsUri& target)
{
    const QString path = target.toLocalPath();
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly))
        return errorForPath(path);
    return Result<std::unique_ptr<QIODevice>>(std::move(file));
}

Result<std::unique_ptr<QIODevice>> LocalFileSystem::openWrite(const VfsUri& target)
{
    const QString path = target.toLocalPath();
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return VfsError::make(VfsError::IoError, QStringLiteral("Cannot write %1").arg(path));
    return Result<std::unique_ptr<QIODevice>>(std::move(file));
}

FileSystemPtr LocalFileSystemFactory::create(const QVariantMap&, QString*)
{
    return std::make_shared<LocalFileSystem>();
}

} // namespace mole
