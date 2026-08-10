#include "plugins/archive/ArchiveFileSystem.h"

#include <QBuffer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QUrl>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>

namespace mole {
namespace {

    constexpr int kReadBlockSize = 64 * 1024;

    /// RAII for libarchive's read handle, so no early return can leak it.
    class ArchiveReader
    {
    public:
        explicit ArchiveReader(const QString& path)
        {
            m_handle = archive_read_new();
            if (!m_handle)
                return;
            archive_read_support_filter_all(m_handle);
            archive_read_support_format_all(m_handle);
            m_opened = archive_read_open_filename(m_handle, path.toLocal8Bit().constData(), kReadBlockSize)
                == ARCHIVE_OK;
        }

        ~ArchiveReader()
        {
            if (!m_handle)
                return;
            if (m_opened)
                archive_read_close(m_handle);
            archive_read_free(m_handle);
        }

        ArchiveReader(const ArchiveReader&) = delete;
        ArchiveReader& operator=(const ArchiveReader&) = delete;

        bool isOpen() const { return m_handle && m_opened; }
        struct archive* handle() const { return m_handle; }
        QString errorText() const
        {
            const char* message = m_handle ? archive_error_string(m_handle) : nullptr;
            return message ? QString::fromLocal8Bit(message) : QStringLiteral("unknown archive error");
        }

    private:
        struct archive* m_handle = nullptr;
        bool m_opened = false;
    };

    /// libarchive reports "dir/sub/file.txt"; normalise to "/dir/sub/file.txt".
    ///
    /// "." and ".." are resolved here and stop at the root, which is the whole
    /// of this mount's defence against a hostile archive. An entry called
    /// "../../etc/passwd" is an instruction to write outside wherever it is
    /// extracted to, and one called "/etc/passwd" is the same instruction
    /// spelled differently; both become paths inside the archive and can address
    /// nothing else. It is also what keeps the tree acyclic -- "/.." resolves to
    /// the root, so a directory listing that contained it would list itself, and
    /// anything walking the mount would go round for ever.
    QString normaliseEntryPath(const char* raw)
    {
        QString raw_path = QString::fromUtf8(raw);
        raw_path.replace(QLatin1Char('\\'), QLatin1Char('/'));

        QStringList parts;
        for (const QString& part : raw_path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (part == QLatin1String("."))
                continue;
            if (part == QLatin1String("..")) {
                if (!parts.isEmpty())
                    parts.removeLast();
                continue;
            }
            parts.append(part);
        }
        if (parts.isEmpty())
            return {};
        return QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }

} // namespace

ArchiveFileSystem::ArchiveFileSystem(QString archivePath)
    : m_archivePath(std::move(archivePath))
{
}

QString ArchiveFileSystem::authorityFor(const QString& archivePath)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(archivePath));
}

QString ArchiveFileSystem::archivePathFromAuthority(const QString& authority)
{
    return QUrl::fromPercentEncoding(authority.toLatin1());
}

VfsCapabilities ArchiveFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::RandomAccessRead;
}

Result<void> ArchiveFileSystem::ensureIndexed()
{
    // Caller holds m_mutex.
    if (m_indexed)
        return {};

    if (!QFileInfo::exists(m_archivePath)) {
        return Result<void>::failure(
            VfsError::NotFound, QStringLiteral("Archive not found: %1").arg(m_archivePath));
    }

    ArchiveReader reader(m_archivePath);
    if (!reader.isOpen()) {
        return Result<void>::failure(VfsError::IoError,
            QStringLiteral("Cannot open archive %1: %2").arg(m_archivePath, reader.errorText()));
    }

    m_nodes.insert(QStringLiteral("/"), Node { true, 0, QFileInfo(m_archivePath).lastModified() });

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.handle(), &entry) == ARCHIVE_OK) {
        const QString path = normaliseEntryPath(archive_entry_pathname(entry));
        if (path.isEmpty())
            continue;

        Node node;
        node.isDir = archive_entry_filetype(entry) == AE_IFDIR;
        node.size = node.isDir ? 0 : static_cast<qint64>(archive_entry_size(entry));
        if (archive_entry_mtime_is_set(entry))
            node.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
        m_nodes.insert(path, node);

        // Many archives omit directory records entirely, so synthesise every
        // parent or the tree would have holes in it.
        int slash = path.lastIndexOf(QLatin1Char('/'));
        while (slash > 0) {
            const QString parent = path.left(slash);
            if (m_nodes.contains(parent))
                break;
            m_nodes.insert(parent, Node { true, 0, node.modified });
            slash = parent.lastIndexOf(QLatin1Char('/'));
        }

        archive_read_data_skip(reader.handle());
    }

    m_indexed = true;
    return {};
}

Result<FileEntryList> ArchiveFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> indexed = ensureIndexed(); !indexed.ok())
        return indexed.error();
    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled"));

    const QString path = dir.path();
    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such directory: %1").arg(path));
    if (!node->isDir)
        return VfsError::make(VfsError::NotADirectory, QStringLiteral("Not a directory: %1").arg(path));

    const QString prefix = (path == QLatin1String("/")) ? QStringLiteral("/") : path + QLatin1Char('/');

    FileEntryList out;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        const QString& candidate = it.key();
        if (candidate == path || !candidate.startsWith(prefix))
            continue;
        if (candidate.indexOf(QLatin1Char('/'), prefix.size()) >= 0)
            continue;

        FileEntry file;
        file.name = candidate.mid(prefix.size());
        file.uri = VfsUri(scheme(), dir.authority(), candidate);
        file.isDir = it->isDir;
        file.isHidden = file.name.startsWith(QLatin1Char('.'));
        file.isWritable = false;
        file.size = it->size;
        file.modified = it->modified;
        out.append(file);
    }

    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return out;
}

Result<FileEntry> ArchiveFileSystem::stat(const VfsUri& target)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> indexed = ensureIndexed(); !indexed.ok())
        return indexed.error();

    const auto node = m_nodes.constFind(target.path());
    if (node == m_nodes.constEnd())
        return VfsError::make(
            VfsError::NotFound, QStringLiteral("No such entry in archive: %1").arg(target.path()));

    FileEntry entry;
    entry.name = target.fileName();
    entry.uri = target;
    entry.isDir = node->isDir;
    entry.isWritable = false;
    entry.size = node->size;
    entry.modified = node->modified;
    return entry;
}

Result<std::unique_ptr<QIODevice>> ArchiveFileSystem::openRead(const VfsUri& target, qint64)
{
    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> indexed = ensureIndexed(); !indexed.ok())
            return indexed.error();

        const auto node = m_nodes.constFind(target.path());
        if (node == m_nodes.constEnd())
            return VfsError::make(
                VfsError::NotFound, QStringLiteral("No such entry in archive: %1").arg(target.path()));
        if (node->isDir)
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("Is a directory: %1").arg(target.path()));
    }

    // Stream formats have no random access, so extraction means scanning from
    // the start. Fine for previews; a large extraction belongs in a Task.
    ArchiveReader reader(m_archivePath);
    if (!reader.isOpen()) {
        return VfsError::make(VfsError::IoError,
            QStringLiteral("Cannot open archive %1: %2").arg(m_archivePath, reader.errorText()));
    }

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.handle(), &entry) == ARCHIVE_OK) {
        if (normaliseEntryPath(archive_entry_pathname(entry)) != target.path()) {
            archive_read_data_skip(reader.handle());
            continue;
        }

        QByteArray contents;
        QByteArray chunk(kReadBlockSize, Qt::Uninitialized);
        for (;;) {
            const la_ssize_t read = archive_read_data(reader.handle(), chunk.data(), kReadBlockSize);
            if (read < 0) {
                return VfsError::make(VfsError::IoError,
                    QStringLiteral("Cannot read %1: %2").arg(target.path(), reader.errorText()));
            }
            if (read == 0)
                break;
            contents.append(chunk.constData(), static_cast<int>(read));
        }

        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(contents);
        if (!buffer->open(QIODevice::ReadOnly)) {
            return VfsError::make(VfsError::IoError, QStringLiteral("Cannot buffer %1").arg(target.path()));
        }
        return Result<std::unique_ptr<QIODevice>>(std::move(buffer));
    }

    return VfsError::make(
        VfsError::NotFound, QStringLiteral("Entry vanished from archive: %1").arg(target.path()));
}

QList<ConnectionField> ArchiveFileSystemFactory::connectionFields() const
{
    ConnectionField path;
    path.key = QStringLiteral("path");
    path.label = QStringLiteral("Archive file");
    path.kind = ConnectionField::Text;
    path.required = true;
    return { path };
}

FileSystemPtr ArchiveFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const QString path = config.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An archive mount needs a 'path'");
        return nullptr;
    }
    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("No such archive: %1").arg(path);
        return nullptr;
    }
    return std::make_shared<ArchiveFileSystem>(path);
}

QVariantMap ArchiveFileSystemFactory::configForFile(const QString& localPath) const
{
    return { { QStringLiteral("path"), localPath },
        { QStringLiteral("authority"), ArchiveFileSystem::authorityFor(localPath) },
        { QStringLiteral("rootPath"), QStringLiteral("/") } };
}

VfsUri ArchiveFileSystemFactory::rootUriForFile(const QString& localPath) const
{
    return VfsUri(QStringLiteral("archive"), ArchiveFileSystem::authorityFor(localPath), QStringLiteral("/"));
}

QStringList ArchiveFileSystemFactory::supportedSuffixes()
{
    // libarchive handles more than this; the list is what we advertise as
    // "double-click to open as a drive".
    return { QStringLiteral("zip"), QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("tgz"),
        QStringLiteral("bz2"), QStringLiteral("xz"), QStringLiteral("zst"), QStringLiteral("7z"),
        QStringLiteral("rar"), QStringLiteral("iso"), QStringLiteral("cpio"), QStringLiteral("ar") };
}

bool ArchiveFileSystemFactory::looksLikeArchive(const QString& fileName)
{
    const QString lower = fileName.toLower();
    const QStringList suffixes = supportedSuffixes();
    return std::any_of(suffixes.begin(), suffixes.end(),
        [&lower](const QString& suffix) { return lower.endsWith(QLatin1Char('.') + suffix); });
}

} // namespace mole
