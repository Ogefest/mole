#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include <algorithm>

namespace mole {

MemoryFileSystem::MemoryFileSystem()
{
    m_nodes.insert(QStringLiteral("/"), Node { true, {}, QDateTime::currentDateTime() });
}

VfsCapabilities MemoryFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString MemoryFileSystem::normalise(const QString& path)
{
    return VfsUri(QStringLiteral("mem"), QString(), path).path();
}

VfsUri MemoryFileSystem::uriFor(const QString& path) const
{
    return VfsUri(QStringLiteral("mem"), QString(), path);
}

Result<void> MemoryFileSystem::faultFor(const QString& path) const
{
    const auto it = m_faults.constFind(path);
    if (it == m_faults.constEnd() || *it == VfsError::None)
        return {};
    return Result<void>::failure(*it, QStringLiteral("Injected fault on %1").arg(path));
}

void MemoryFileSystem::addDirectory(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    const QString normalised = normalise(path);

    QString accumulated;
    for (const QString& part : normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        accumulated += QLatin1Char('/') + part;
        if (!m_nodes.contains(accumulated))
            m_nodes.insert(accumulated, Node { true, {}, QDateTime::currentDateTime() });
    }
}

void MemoryFileSystem::addFile(const QString& path, const QByteArray& contents, const QDateTime& modified)
{
    const QString normalised = normalise(path);
    const int slash = normalised.lastIndexOf(QLatin1Char('/'));
    if (slash > 0)
        addDirectory(normalised.left(slash));

    QMutexLocker lock(&m_mutex);
    m_nodes.insert(
        normalised, Node { false, contents, modified.isValid() ? modified : QDateTime::currentDateTime() });
}

void MemoryFileSystem::setFault(const QString& path, VfsError::Code error)
{
    QMutexLocker lock(&m_mutex);
    if (error == VfsError::None)
        m_faults.remove(normalise(path));
    else
        m_faults.insert(normalise(path), error);
}

void MemoryFileSystem::clearFaults()
{
    QMutexLocker lock(&m_mutex);
    m_faults.clear();
}

int MemoryFileSystem::listCallCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_listCalls;
}

Result<FileEntryList> MemoryFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    if (m_listDelayMs > 0) {
        // Chunked so a cancelled task does not have to wait out the full delay.
        for (int slept = 0; slept < m_listDelayMs && !cancel.isCancelled(); slept += 10)
            QThread::msleep(10);
    }
    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled"));

    QMutexLocker lock(&m_mutex);
    ++m_listCalls;

    const QString path = dir.path();
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault.error();

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
        // Direct children only.
        if (candidate.indexOf(QLatin1Char('/'), prefix.size()) >= 0)
            continue;

        FileEntry entry;
        entry.name = candidate.mid(prefix.size());
        entry.uri = uriFor(candidate);
        entry.isDir = it->isDir;
        entry.isHidden = entry.name.startsWith(QLatin1Char('.'));
        entry.isWritable = true;
        entry.size = it->isDir ? 0 : it->contents.size();
        entry.modified = it->modified;
        out.append(entry);
    }

    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return out;
}

Result<FileEntry> MemoryFileSystem::stat(const VfsUri& target)
{
    QMutexLocker lock(&m_mutex);
    const QString path = target.path();
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault.error();

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));

    FileEntry entry;
    entry.name = target.fileName();
    entry.uri = target;
    entry.isDir = node->isDir;
    entry.isWritable = true;
    entry.size = node->isDir ? 0 : node->contents.size();
    entry.modified = node->modified;
    return entry;
}

Result<void> MemoryFileSystem::makeDirectory(const VfsUri& target)
{
    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> fault = faultFor(target.path()); !fault.ok())
            return fault;
        if (m_nodes.contains(target.path()))
            return Result<void>::failure(
                VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(target.path()));
    }
    addDirectory(target.path());
    return {};
}

Result<void> MemoryFileSystem::remove(const VfsUri& target, bool recursive)
{
    QMutexLocker lock(&m_mutex);
    const QString path = target.path();
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault;

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));

    if (!node->isDir) {
        m_nodes.remove(path);
        return {};
    }

    const QString prefix = path + QLatin1Char('/');
    QStringList children;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        if (it.key().startsWith(prefix))
            children.append(it.key());
    }

    if (!children.isEmpty() && !recursive)
        return Result<void>::failure(VfsError::NotEmpty, QStringLiteral("Directory not empty: %1").arg(path));

    for (const QString& child : children)
        m_nodes.remove(child);
    m_nodes.remove(path);
    return {};
}

Result<void> MemoryFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    QMutexLocker lock(&m_mutex);
    const QString src = from.path();
    const QString dst = to.path();

    if (Result<void> fault = faultFor(src); !fault.ok())
        return fault;
    if (!m_nodes.contains(src))
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("No such file: %1").arg(src));
    if (m_nodes.contains(dst))
        return Result<void>::failure(VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(dst));

    const QString prefix = src + QLatin1Char('/');
    const QList<QString> keys = m_nodes.keys();
    for (const QString& key : keys) {
        if (key == src) {
            m_nodes.insert(dst, m_nodes.take(src));
        } else if (key.startsWith(prefix)) {
            m_nodes.insert(dst + key.mid(src.size()), m_nodes.take(key));
        }
    }
    return {};
}

Result<std::unique_ptr<QIODevice>> MemoryFileSystem::openRead(const VfsUri& target, qint64)
{
    // Slept before the lock is taken, so a delayed read does not block every
    // other caller of this drive for the duration.
    if (m_readDelayMs > 0)
        QThread::msleep(static_cast<unsigned long>(m_readDelayMs));

    QMutexLocker lock(&m_mutex);
    const QString path = target.path();
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault.error();

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));
    if (node->isDir)
        return VfsError::make(VfsError::IsADirectory, QStringLiteral("Is a directory: %1").arg(path));

    auto buffer = std::make_unique<QBuffer>();
    buffer->setData(node->contents);
    if (!buffer->open(QIODevice::ReadOnly))
        return VfsError::make(VfsError::IoError, QStringLiteral("Cannot open %1").arg(path));
    return Result<std::unique_ptr<QIODevice>>(std::move(buffer));
}

namespace {

    /// Buffers the written bytes and commits them into the node map on close.
    /// Holds a weak reference so a device outliving its filesystem is a no-op
    /// rather than a crash.
    class MemoryWriteDevice final : public QBuffer
    {
    public:
        MemoryWriteDevice(std::weak_ptr<MemoryFileSystem> owner, QString path)
            : m_owner(std::move(owner))
            , m_path(std::move(path))
        {
        }

        // Nothing lands here. A device destroyed without being closed is an
        // abandoned write -- a cancelled copy, or one that failed part way --
        // and committing it would put a half file under the name somebody asked
        // for. Every backend agrees about that, whatever it does underneath:
        // the disk writes under a working name and removes it, and a scratch
        // drive in RAM simply forgets. See ADR-0021.
        ~MemoryWriteDevice() override = default;

        void close() override
        {
            QBuffer::close();
            commit();
        }

    private:
        void commit()
        {
            if (m_committed)
                return;
            m_committed = true;
            if (auto owner = m_owner.lock())
                owner->addFile(m_path, data());
        }

        std::weak_ptr<MemoryFileSystem> m_owner;
        QString m_path;
        bool m_committed = false;
    };

} // namespace

Result<std::unique_ptr<QIODevice>> MemoryFileSystem::openWrite(const VfsUri& target, qint64)
{
    // A stack-allocated MemoryFileSystem has no owning shared_ptr, and a device
    // that outlives its filesystem would be a dangling write.
    std::weak_ptr<MemoryFileSystem> owner = weak_from_this();
    if (owner.expired()) {
        return VfsError::make(VfsError::NotSupported,
            QStringLiteral("mem:// writes need the filesystem to be held by a shared_ptr"));
    }

    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> fault = faultFor(target.path()); !fault.ok())
            return fault.error();

        const auto existing = m_nodes.constFind(target.path());
        if (existing != m_nodes.constEnd() && existing->isDir) {
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("Is a directory: %1").arg(target.path()));
        }
    }

    auto device = std::make_unique<MemoryWriteDevice>(std::move(owner), target.path());
    if (!device->open(QIODevice::WriteOnly)) {
        return VfsError::make(VfsError::IoError, QStringLiteral("Cannot write %1").arg(target.path()));
    }
    return Result<std::unique_ptr<QIODevice>>(std::move(device));
}

FileSystemPtr MemoryFileSystemFactory::create(const QVariantMap&, QString*)
{
    return std::make_shared<MemoryFileSystem>();
}

} // namespace mole
