#include "core/vfs/backends/LocalFileSystem.h"

#include "core/vfs/PartialWrite.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

namespace mole {
namespace {

    /// A file written under a working name and moved into place when it is
    /// closed.
    ///
    /// The rename is the only instant at which the destination changes, which is
    /// what makes an interrupted write leave the previous contents alone rather
    /// than in pieces. Its failure is the write's failure — every byte arriving
    /// and the file not being there is not a success with a caveat.
    class PartialFile final : public QFile, public ICommitsOnClose
    {
    public:
        PartialFile(IFileSystem& owner, VfsUri staging, VfsUri target)
            : QFile(staging.toLocalPath())
            , m_owner(owner)
            , m_staging(std::move(staging))
            , m_target(std::move(target))
            // Looked at now, before a byte is written, because that is the only
            // moment at which "it exists" means "the caller is overwriting
            // something it knew about" rather than "something turned up".
            , m_replacing(QFileInfo::exists(m_target.toLocalPath()))
        {
        }

        ~PartialFile() override
        {
            // Destroyed without being closed is an abandoned write — cancelled,
            // or the caller gave up. What was written is not a file anybody
            // asked for, so it goes rather than being left to be puzzled over.
            if (m_committed)
                return;

            // Settled *before* anything is done to the file, because
            // QFile::remove() closes it first and close() is virtual: the call
            // arrives back in this class, finds a write that has not committed,
            // and puts it in place — renaming a cancelled copy into the name
            // somebody asked for, which is the one outcome the working name
            // exists to prevent.
            m_committed = true;
            QFile::close();
            QFile::remove();
        }

        VfsError commitError() const override { return m_error; }

        void close() override
        {
            if (m_committed) {
                QFile::close();
                return;
            }
            m_committed = true;

            const bool flushed = flush();
            QFile::close();
            if (!flushed) {
                m_error = VfsError::make(VfsError::IoError,
                    QStringLiteral("Could not finish writing %1").arg(m_target.toLocalPath()));
                QFile::remove();
                setErrorString(m_error.message);
                return;
            }

            m_error = commitPartialWrite(m_owner, m_staging, m_target, m_replacing);
            if (m_error.isError())
                setErrorString(m_error.message);
        }

    private:
        IFileSystem& m_owner;
        VfsUri m_staging;
        VfsUri m_target;
        bool m_replacing = false;
        VfsError m_error;
        bool m_committed = false;
    };

    FileEntry entryFromInfo(const QFileInfo& info, const VfsUri& uri)
    {
        FileEntry e;
        e.name = info.fileName();
        e.uri = uri;
        e.isDir = info.isDir();
        // isSymLink() is true for three different things on Windows: an NTFS
        // symbolic link, a junction, and a .lnk shortcut -- which is not a link
        // at all. Qt separates the questions and this asks the one it means. A
        // junction is close enough to a link to follow the same rule; a shortcut
        // is a file.
        e.isSymlink = info.isSymbolicLink() || info.isJunction();
        e.isShortcut = info.isShortcut();
        e.isHidden = info.isHidden();
        e.isReadable = info.isReadable();
        e.isWritable = info.isWritable();
        e.size = info.isDir() ? 0 : info.size();
        e.modified = info.lastModified();
        // Both are left invalid where the filesystem does not keep them, which
        // is what QFileInfo already answers: birthTime is empty on filesystems
        // with no birth time, and lastRead is the mount's business. A search by
        // either has to see the absence rather than a date that means nothing.
        e.created = info.birthTime();
        e.accessed = info.lastRead();
        e.permissions = LocalFileSystem::modeString(info.permissions());
        return e;
    }

    /// Whether two names, both of which exist, are names of the same node.
    ///
    /// This is the question a rename guard actually wants. Asking whether the
    /// destination name is taken gets "yes" for a rename of report.txt to
    /// Report.txt on any volume that does not distinguish case -- the file in
    /// the way is the file being renamed.
    ///
    /// A link is excluded rather than resolved, and that is the whole reason
    /// this is not one call to canonicalFilePath(). A symbolic link and its
    /// target share a canonical path, so renaming a link onto what it points at
    /// would look like the same node and would replace the target with the link,
    /// destroying the only copy of its contents.
    bool namesTheSameNode(const QString& first, const QString& second)
    {
        const QFileInfo a(first);
        const QFileInfo b(second);
        if (a.isSymLink() || b.isSymLink())
            return false;

        const QString canonicalA = a.canonicalFilePath();
        const QString canonicalB = b.canonicalFilePath();
        return !canonicalA.isEmpty() && canonicalA == canonicalB;
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

QString LocalFileSystem::modeString(QFile::Permissions permissions, HostPlatform platform)
{
    // Windows has no mode. QFileInfo::permissions() synthesises something from
    // the ACL, and written out as nine characters it reads as fact -- so it is
    // offered only where it means what it says. AlertEvaluator depends on this
    // being empty rather than invented: it treats an empty string as "this drive
    // does not report permissions" and skips the reading, and a synthesised
    // value would have an alert rule firing on a mode nobody ever set.
    if (platform == HostPlatform::Windows)
        return {};

    // "rwxr-xr--", the form everyone reads permissions in. Built from Qt's
    // owner/group/other flags rather than a raw octal number so a change is
    // legible in an alert without decoding anything.
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
    const QStorageInfo storage(target.toLocalPath());
    if (!storage.isValid() || !storage.isReady()) {
        return VfsError::make(
            VfsError::NotFound, QStringLiteral("No volume is mounted at %1").arg(target.toLocalPath()));
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
    const QString path = target.toLocalPath();
    const QFileInfo info(path);
    if (!info.exists())
        return errorForPath(path);

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

    // Whether there is a mode string at all is one question with one answer, and
    // the listing asks the same one. It used to be guarded here and unguarded
    // there, which is two answers about the same file.
    out.nativeText = modeString(info.permissions());

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
    // A link is a name, and removing the name is the whole job. Asked whether it
    // is a directory, a link to one says yes -- so a recursive delete that looks
    // no further walks through it and empties whatever it points at, which is
    // how deleting a scratch folder takes a home directory with it. Checked
    // before existence as well, because a link whose target is gone does not
    // "exist" and still has to be removable.
    if (info.isSymLink()) {
        if (!QFile::remove(path))
            return Result<void>::failure(VfsError::IoError, QStringLiteral("Cannot remove %1").arg(path));
        return {};
    }

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
    // Something being there is only a collision when it is something else. On a
    // case-insensitive volume -- every NTFS one, and an APFS one by default --
    // renaming report.txt to Report.txt finds the file being renamed sitting in
    // its own way, and there was no way to make it happen at all.
    //
    // The check itself cannot simply go: POSIX rename() overwrites the
    // destination silently, and this is what stops a rename destroying a file
    // nobody mentioned.
    if (QFileInfo::exists(dst) && !namesTheSameNode(src, dst))
        return Result<void>::failure(VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(dst));
    if (!QFile::rename(src, dst))
        return Result<void>::failure(
            VfsError::IoError, QStringLiteral("Cannot rename %1 to %2").arg(src, dst));
    return {};
}

Result<std::unique_ptr<QIODevice>> LocalFileSystem::openRead(const VfsUri& target, qint64)
{
    const QString path = target.toLocalPath();
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly))
        return errorForPath(path);
    return Result<std::unique_ptr<QIODevice>>(std::move(file));
}

Result<std::unique_ptr<QIODevice>> LocalFileSystem::openWrite(const VfsUri& target, qint64)
{
    // Under a working name until it is finished, and renamed into place at the
    // end. See ADR-0021.
    //
    // Writing straight to the destination was two faults rather than one. A
    // process killed part way through left half a file under the name somebody
    // asked for, indistinguishable from a file that was simply that size — and
    // Truncate had already destroyed whatever was there before the first byte of
    // the replacement arrived, so an interrupted overwrite lost the original as
    // well as failing to produce the new one.
    const VfsUri staging = partialWriteOf(target);
    auto file = std::make_unique<PartialFile>(*this, staging, target);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return VfsError::make(VfsError::IoError,
            QStringLiteral("Cannot write %1: %2").arg(target.toLocalPath(), file->errorString()));
    }
    return Result<std::unique_ptr<QIODevice>>(std::move(file));
}

FileSystemPtr LocalFileSystemFactory::create(const QVariantMap&, QString*)
{
    return std::make_shared<LocalFileSystem>();
}

} // namespace mole
