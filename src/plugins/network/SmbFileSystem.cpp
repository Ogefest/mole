#include "plugins/network/SmbFileSystem.h"

#include <QDateTime>
#include <QIODevice>
#include <QThread>

#include <cstring>
#include <libsmbclient.h>

namespace mole {
namespace {

    constexpr QChar kSeparator = QLatin1Char('/');

    /// libsmbclient reports failures through errno, like the C library it is
    /// modelled on. Turned into something the interface can act on, because
    /// "operation failed" is the one answer nobody can do anything with.
    VfsError errorFromErrno(int code, const QString& what)
    {
        switch (code) {
        case ENOENT:
            return VfsError::make(VfsError::NotFound, QStringLiteral("%1: there is nothing there").arg(what));
        case EACCES:
        case EPERM:
            return VfsError::make(VfsError::AccessDenied, QStringLiteral("%1: permission denied").arg(what));
        case EEXIST:
            return VfsError::make(
                VfsError::AlreadyExists, QStringLiteral("%1: there is already something there").arg(what));
        case ENOTEMPTY:
            return VfsError::make(
                VfsError::NotEmpty, QStringLiteral("%1: the directory is not empty").arg(what));
        case ENOSPC:
            return VfsError::make(VfsError::IoError, QStringLiteral("%1: the share is full").arg(what));
        case ENOTDIR:
            return VfsError::make(
                VfsError::NotADirectory, QStringLiteral("%1: that is not a directory").arg(what));
        case EISDIR:
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("%1: that is a directory").arg(what));
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ETIMEDOUT:
            return VfsError::make(
                VfsError::NetworkError, QStringLiteral("%1: the server did not answer").arg(what));
        case EINVAL:
            // What libsmbclient answers for a name the protocol will not carry,
            // which is a different thing from a network that is down.
            return VfsError::make(
                VfsError::IoError, QStringLiteral("%1: the server refused that name").arg(what));
        default:
            break;
        }
        return VfsError::make(VfsError::IoError,
            QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(std::strerror(code))));
    }

    /// A file on the share, as a QIODevice.
    ///
    /// Seekable, which is what lets SMB advertise RandomAccessRead: a preview
    /// reads one page from the middle of a file rather than the file.
    class SmbFile final : public QIODevice
    {
    public:
        SmbFile(SMBCCTX* context, SMBCFILE* handle, qint64 size, QString what)
            : m_context(context)
            , m_handle(handle)
            , m_size(size)
            , m_what(std::move(what))
        {
        }

        ~SmbFile() override { closeHandle(); }

        bool isSequential() const override { return false; }
        qint64 size() const override { return m_size >= 0 ? m_size : QIODevice::size(); }

        void close() override
        {
            QIODevice::close();
            closeHandle();
        }

        bool seek(qint64 position) override
        {
            if (position < 0)
                return false;
            const off_t landed = smbc_getFunctionLseek(m_context)(m_context, m_handle, position, SEEK_SET);
            if (landed < 0) {
                setErrorString(errorFromErrno(errno, m_what).message);
                return false;
            }
            return QIODevice::seek(position);
        }

        /// Meaningful once a read has answered -1.
        VfsError failure() const { return m_failure; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            if (maxSize <= 0)
                return 0;
            const ssize_t got
                = smbc_getFunctionRead(m_context)(m_context, m_handle, data, static_cast<size_t>(maxSize));
            if (got < 0) {
                m_failure = errorFromErrno(errno, m_what);
                setErrorString(m_failure.message);
                return -1;
            }
            return got;
        }

        qint64 writeData(const char* data, qint64 size) override
        {
            if (size <= 0)
                return 0;
            const ssize_t put = smbc_getFunctionWrite(m_context)(
                m_context, m_handle, const_cast<char*>(data), static_cast<size_t>(size));
            if (put < 0) {
                m_failure = errorFromErrno(errno, m_what);
                setErrorString(m_failure.message);
                return -1;
            }
            return put;
        }

    private:
        void closeHandle()
        {
            if (!m_handle)
                return;
            smbc_getFunctionClose(m_context)(m_context, m_handle);
            m_handle = nullptr;
        }

        SMBCCTX* m_context = nullptr;
        SMBCFILE* m_handle = nullptr;
        qint64 m_size = -1;
        QString m_what;
        VfsError m_failure;
    };

} // namespace

QString SmbSettings::origin() const
{
    QString bare = host.trimmed();
    while (bare.endsWith(kSeparator))
        bare.chop(1);
    return QStringLiteral("smb://") + bare + kSeparator + share;
}

/// What the authentication callback is handed back, because libsmbclient gives
/// a callback nothing but the pointer it was told to keep.
struct SmbFileSystem::Credentials
{
    QByteArray username;
    QByteArray password;
    QByteArray domain;
};

namespace {

    void supplyCredentials(SMBCCTX* context, const char*, const char*, char* workgroup, int workgroupLength,
        char* username, int usernameLength, char* password, int passwordLength)
    {
        const auto* credentials
            = static_cast<const SmbFileSystem::Credentials*>(smbc_getOptionUserData(context));
        if (!credentials)
            return;
        // Truncated rather than overrun: libsmbclient hands over fixed buffers
        // and takes no responsibility for what is put in them.
        const auto copyInto = [](char* into, int room, const QByteArray& from) {
            if (room <= 0)
                return;
            const int fits = qMin(room - 1, static_cast<int>(from.size()));
            std::memcpy(into, from.constData(), static_cast<size_t>(fits));
            into[fits] = '\0';
        };
        copyInto(workgroup, workgroupLength, credentials->domain);
        copyInto(username, usernameLength, credentials->username);
        copyInto(password, passwordLength, credentials->password);
    }

} // namespace

SmbFileSystem::SmbFileSystem(QString scheme, SmbSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
    , m_credentials(std::make_unique<Credentials>())
{
    m_credentials->username = m_settings.username.toUtf8();
    m_credentials->password = m_settings.password.toUtf8();
    m_credentials->domain = m_settings.domain.toUtf8();
}

SmbFileSystem::~SmbFileSystem() = default;

SMBCCTX* SmbFileSystem::context() const
{
    // One per thread, built on first use and then kept for the life of the
    // thread. See the note on the class for why it is per thread; why it is
    // never freed is a separate lesson, learned the hard way.
    //
    // `smbc_free_context()` tears down state libsmbclient keeps *globally*, not
    // per context. Freeing one while the process still means to use another --
    // which is what happens the moment a second drive is configured, or the same
    // drive is rebuilt -- aborts inside Samba's allocator: "talloc: access after
    // free ... source3/param/loadparm.c". So a thread's context outlives every
    // drive that used it, and the thread taking it away is the only teardown.
    // A pool has a bounded number of threads, so this is a bounded amount of
    // memory rather than a leak that grows.
    static thread_local SMBCCTX* held = nullptr;

    if (!held) {
        SMBCCTX* fresh = smbc_new_context();
        if (!fresh)
            return nullptr;

        smbc_setFunctionAuthDataWithContext(fresh, supplyCredentials);
        smbc_setOptionUseKerberos(fresh, 0);
        smbc_setOptionFallbackAfterKerberos(fresh, 1);

        // The dialect is left to Samba's own configuration rather than set here.
        // smbc_setOptionProtocols() takes `char*` and the library takes an
        // interest in the memory afterwards; handing it a string literal
        // corrupted the heap a few operations later, inside its own allocator.
        // Samba has not offered SMB1 by default for years, so the setting bought
        // nothing that the default does not already give.

        if (!smbc_init_context(fresh)) {
            smbc_free_context(fresh, 1);
            return nullptr;
        }
        held = fresh;
    }

    // The credentials belong to the drive asking rather than to the thread, and
    // one thread serves every drive in turn -- so they are set on the way in to
    // every call rather than once when the context is built. Sequential on this
    // thread by construction: a backend call does not return until it is done.
    smbc_setOptionUserData(held, m_credentials.get());
    return held;
}

VfsCapabilities SmbFileSystem::capabilities() const
{
    // No ReportsSpace yet: libsmbclient offers statvfs and not every server
    // answers it honestly, and a capacity that is wrong is read as fact.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString SmbFileSystem::urlFor(const VfsUri& uri) const
{
    QString root = m_settings.remoteRoot;
    while (root.endsWith(kSeparator))
        root.chop(1);
    while (root.startsWith(kSeparator))
        root.remove(0, 1);

    QString path = uri.path();
    while (path.startsWith(kSeparator))
        path.remove(0, 1);

    QString joined = m_settings.origin();
    if (!root.isEmpty())
        joined += kSeparator + root;
    if (!path.isEmpty())
        joined += kSeparator + path;
    return joined;
}

Result<FileEntryList> SmbFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    SMBCCTX* ctx = context();
    if (!ctx) {
        return Result<FileEntryList>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Listing %1").arg(dir.path());
    const QByteArray url = urlFor(dir).toUtf8();
    SMBCFILE* handle = smbc_getFunctionOpendir(ctx)(ctx, url.constData());
    if (!handle)
        return Result<FileEntryList>(errorFromErrno(errno, what));

    FileEntryList entries;
    while (struct smbc_dirent* found = smbc_getFunctionReaddir(ctx)(ctx, handle)) {
        if (cancel.isCancelled()) {
            smbc_getFunctionClosedir(ctx)(ctx, handle);
            return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));
        }

        // From namelen, and never from the pointer alone. `smbc_dirent` ends in
        // a flexible array declared `char name[1]`, so the name is as long as
        // namelen says and the array's *type* claims one byte -- which is enough
        // for QString::fromUtf8 to bind to its QByteArrayView overload, take the
        // array's extent, and hand back a one-character name. Every file in
        // every listing came back as its own first letter.
        const QString name = QString::fromUtf8(found->name, qstrnlen(found->name, found->namelen));
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        // Printers, IPC endpoints and the rest of what a server offers. They are
        // not files and nothing in a file manager can do anything with them.
        if (found->smbc_type != SMBC_FILE && found->smbc_type != SMBC_DIR)
            continue;

        FileEntry entry;
        entry.name = name;
        entry.uri = dir.child(name);
        entry.isDir = found->smbc_type == SMBC_DIR;

        // Size and time need a stat of their own: a directory entry carries the
        // name and the kind and nothing else. Asked for here rather than left
        // out, because a listing without sizes or dates is half a listing.
        struct stat details
        {
        };
        const QByteArray childUrl = urlFor(entry.uri).toUtf8();
        if (smbc_getFunctionStat(ctx)(ctx, childUrl.constData(), &details) == 0) {
            entry.size = entry.isDir ? 0 : static_cast<qint64>(details.st_size);
            entry.modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(details.st_mtime));
        }
        entries.append(entry);
    }
    smbc_getFunctionClosedir(ctx)(ctx, handle);
    return Result<FileEntryList>(entries);
}

Result<FileEntry> SmbFileSystem::stat(const VfsUri& target)
{
    SMBCCTX* ctx = context();
    if (!ctx) {
        return Result<FileEntry>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QByteArray url = urlFor(target).toUtf8();
    struct stat details
    {
    };
    if (smbc_getFunctionStat(ctx)(ctx, url.constData(), &details) != 0)
        return Result<FileEntry>(errorFromErrno(errno, QStringLiteral("Looking at %1").arg(target.path())));

    FileEntry entry;
    entry.uri = target;
    entry.name = target.fileName();
    entry.isDir = S_ISDIR(details.st_mode);
    entry.size = entry.isDir ? 0 : static_cast<qint64>(details.st_size);
    entry.modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(details.st_mtime));
    return Result<FileEntry>(entry);
}

Result<void> SmbFileSystem::makeDirectory(const VfsUri& target)
{
    SMBCCTX* ctx = context();
    if (!ctx)
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));

    const QByteArray url = urlFor(target).toUtf8();
    if (smbc_getFunctionMkdir(ctx)(ctx, url.constData(), 0755) != 0)
        return errorFromErrno(errno, QStringLiteral("Creating %1").arg(target.path()));
    return {};
}

Result<void> SmbFileSystem::remove(const VfsUri& target, bool recursive)
{
    SMBCCTX* ctx = context();
    if (!ctx)
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));

    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (!what.value().isDir) {
        const QByteArray url = urlFor(target).toUtf8();
        if (smbc_getFunctionUnlink(ctx)(ctx, url.constData()) != 0)
            return errorFromErrno(errno, QStringLiteral("Removing %1").arg(target.path()));
        return {};
    }

    if (recursive) {
        // Depth first, because a directory cannot go until what is in it has.
        const Result<FileEntryList> inside = list(target, CancelToken());
        if (!inside.ok())
            return Result<void>(inside.error());
        for (const FileEntry& child : inside.value()) {
            const Result<void> gone = remove(child.uri, true);
            if (!gone.ok())
                return gone;
        }
    }

    const QByteArray url = urlFor(target).toUtf8();
    if (smbc_getFunctionRmdir(ctx)(ctx, url.constData()) != 0)
        return errorFromErrno(errno, QStringLiteral("Removing %1").arg(target.path()));
    return {};
}

Result<void> SmbFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    SMBCCTX* ctx = context();
    if (!ctx)
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));

    const QByteArray fromUrl = urlFor(from).toUtf8();
    const QByteArray toUrl = urlFor(to).toUtf8();
    if (smbc_getFunctionRename(ctx)(ctx, fromUrl.constData(), ctx, toUrl.constData()) != 0) {
        return errorFromErrno(errno, QStringLiteral("Renaming %1 to %2").arg(from.path(), to.fileName()));
    }
    return {};
}

Result<std::unique_ptr<QIODevice>> SmbFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    SMBCCTX* ctx = context();
    if (!ctx) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Reading %1").arg(target.path());
    const QByteArray url = urlFor(target).toUtf8();
    SMBCFILE* handle = smbc_getFunctionOpen(ctx)(ctx, url.constData(), O_RDONLY, 0);
    if (!handle)
        return Result<std::unique_ptr<QIODevice>>(errorFromErrno(errno, what));

    // Nothing is staged and nothing is held: the share is read through as it is
    // read from, so a file larger than the local disk is a file this can read.
    // That is what ADR-0014 asks of every backend.
    qint64 length = expectedSize;
    if (length < 0) {
        struct stat details
        {
        };
        if (smbc_getFunctionFstat(ctx)(ctx, handle, &details) == 0)
            length = static_cast<qint64>(details.st_size);
    }

    auto file = std::make_unique<SmbFile>(ctx, handle, length, what);
    if (!file->open(QIODevice::ReadOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

Result<std::unique_ptr<QIODevice>> SmbFileSystem::openWrite(const VfsUri& target, qint64)
{
    SMBCCTX* ctx = context();
    if (!ctx) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Writing %1").arg(target.path());
    const QByteArray url = urlFor(target).toUtf8();
    SMBCFILE* handle = smbc_getFunctionOpen(ctx)(ctx, url.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!handle)
        return Result<std::unique_ptr<QIODevice>>(errorFromErrno(errno, what));

    auto file = std::make_unique<SmbFile>(ctx, handle, -1, what);
    if (!file->open(QIODevice::WriteOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

// ---- the factory -----------------------------------------------------------

QList<ConnectionField> SmbFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Server");
    host.help = QStringLiteral("Name or address of the machine, for example fileserver or 10.0.0.4");
    fields.append(host);

    ConnectionField share;
    share.key = QStringLiteral("share");
    share.label = QStringLiteral("Share");
    share.help = QStringLiteral("The share name, without slashes");
    fields.append(share);

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = QStringLiteral("User");
    user.required = false;
    fields.append(user);

    ConnectionField password;
    password.key = QStringLiteral("password");
    password.label = QStringLiteral("Password");
    password.kind = ConnectionField::Password;
    password.required = false;
    fields.append(password);

    ConnectionField domain;
    domain.key = QStringLiteral("domain");
    domain.label = QStringLiteral("Domain");
    domain.help = QStringLiteral("Only for an Active Directory domain; leave empty otherwise");
    domain.required = false;
    domain.advanced = true;
    fields.append(domain);

    ConnectionField root;
    root.key = QStringLiteral("root");
    root.label = QStringLiteral("Folder inside the share");
    root.required = false;
    root.advanced = true;
    fields.append(root);

    return fields;
}

SmbSettings SmbFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    SmbSettings settings;
    settings.host = config.value(QStringLiteral("host")).toString().trimmed();
    settings.share = config.value(QStringLiteral("share")).toString().trimmed();
    settings.username = config.value(QStringLiteral("user")).toString();
    settings.password = config.value(QStringLiteral("password")).toString();
    settings.domain = config.value(QStringLiteral("domain")).toString();
    settings.remoteRoot = config.value(QStringLiteral("root")).toString();

    // A share pasted as \\server\photos or //server/photos is what people have
    // in front of them, so it is taken rather than refused.
    QString share = settings.share;
    share.replace(QLatin1Char('\\'), kSeparator);
    while (share.startsWith(kSeparator))
        share.remove(0, 1);
    if (settings.host.isEmpty() && share.contains(kSeparator)) {
        settings.host = share.section(kSeparator, 0, 0);
        share = share.section(kSeparator, 1);
    }
    while (share.endsWith(kSeparator))
        share.chop(1);
    settings.share = share;
    return settings;
}

FileSystemPtr SmbFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const SmbSettings settings = settingsFrom(config);
    if (settings.host.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An SMB drive needs a server");
        return nullptr;
    }
    if (settings.share.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An SMB drive needs a share");
        return nullptr;
    }
    return std::make_shared<SmbFileSystem>(QStringLiteral("smb"), settings);
}

} // namespace mole
