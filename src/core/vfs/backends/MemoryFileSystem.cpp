#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace mole {

MemoryFileSystem::MemoryFileSystem()
{
    m_nodes.insert(QStringLiteral("/"), Node { true, {}, QDateTime::currentDateTime() });
}

VfsCapabilities MemoryFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead
        | VfsCapability::Symlink;
}

QString MemoryFileSystem::normalise(const QString& path)
{
    return VfsUri(QStringLiteral("mem"), QString(), path).path();
}

QString MemoryFileSystem::resolve(const QString& path) const
{
    if (m_caseSensitivity == Qt::CaseSensitive || m_nodes.contains(path))
        return path;

    const QString folded = path.toCaseFolded();
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        if (it.key().toCaseFolded() == folded)
            return it.key();
    }
    return path;
}

VfsUri MemoryFileSystem::uriFor(const QString& path) const
{
    return VfsUri(QStringLiteral("mem"), QString(), path);
}

void MemoryFileSystem::waitAsASlowDriveWould() const
{
    // In steps, so a cancelled operation does not have to sit out the whole of
    // it -- and read once: the value is set before a case starts and not while
    // one is running.
    const int total = m_operationDelayMs;
    for (int slept = 0; slept < total; slept += 10)
        QThread::msleep(10);
}

Result<void> MemoryFileSystem::faultFor(const QString& path) const
{
    const auto it = m_faults.constFind(path);
    if (it == m_faults.constEnd() || *it == VfsError::None)
        return {};
    return Result<void>::failure(*it, QStringLiteral("Injected fault on %1").arg(path));
}

void MemoryFileSystem::touchParent(const QString& path)
{
    // A real filesystem moves a directory's modification time when something is
    // added to it or taken out of it, and an incremental scan reads exactly
    // that. A fixture that did not would make the scan look correct here and
    // wrong on a disk.
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString parent = slash > 0 ? path.left(slash) : QStringLiteral("/");
    const auto node = m_nodes.find(parent);
    if (node != m_nodes.end() && node->isDir)
        node->modified = QDateTime::currentDateTime();
}

void MemoryFileSystem::addDirectory(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    const QString normalised = normalise(path);

    QString accumulated;
    for (const QString& part : normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        accumulated += QLatin1Char('/') + part;
        if (!m_nodes.contains(accumulated)) {
            touchParent(accumulated);
            m_nodes.insert(accumulated, Node { true, {}, QDateTime::currentDateTime() });
        }
    }
}

void MemoryFileSystem::addFile(const QString& path, const QByteArray& contents, const QDateTime& modified)
{
    const QString normalised = normalise(path);
    const int slash = normalised.lastIndexOf(QLatin1Char('/'));
    if (slash > 0)
        addDirectory(normalised.left(slash));

    QMutexLocker lock(&m_mutex);
    if (!m_nodes.contains(normalised))
        touchParent(normalised);
    m_nodes.insert(
        normalised, Node { false, contents, modified.isValid() ? modified : QDateTime::currentDateTime() });
}

void MemoryFileSystem::setModified(const QString& path, const QDateTime& when)
{
    QMutexLocker lock(&m_mutex);
    const auto node = m_nodes.find(normalise(path));
    if (node != m_nodes.end())
        node->modified = when;
}

void MemoryFileSystem::markAsSymlink(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    const auto node = m_nodes.find(resolve(normalise(path)));
    if (node != m_nodes.end())
        node->isSymlink = true;
}

void MemoryFileSystem::addSymlink(const QString& path, const QString& target)
{
    const QString normalised = normalise(path);
    QMutexLocker lock(&m_mutex);
    if (!m_nodes.contains(normalised))
        touchParent(normalised);
    Node node { false, QByteArray(), QDateTime::currentDateTime() };
    node.isSymlink = true;
    node.linkTarget = target;
    m_nodes.insert(normalised, node);
}

void MemoryFileSystem::markAsShortcut(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    const auto node = m_nodes.find(resolve(normalise(path)));
    if (node != m_nodes.end())
        node->isShortcut = true;
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

int MemoryFileSystem::probeCallCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_probeCalls;
}

bool MemoryFileSystem::isProbing() const
{
    return m_probing.load();
}

Result<QStringList> MemoryFileSystem::askWhatIsOffered(const VfsUri&, const CancelToken& cancel)
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_probeCalls;
    }

    m_probing.store(true);
    if (m_probeDelayMs > 0) {
        // Chunked like the listing delay, so a cancelled probe does not have to
        // wait out the whole of it.
        for (int slept = 0; slept < m_probeDelayMs && !cancel.isCancelled(); slept += 10)
            QThread::msleep(10);
    }
    m_probing.store(false);

    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Probe cancelled"));
    if (m_probeFault != VfsError::None)
        return VfsError::make(m_probeFault, QStringLiteral("This drive cannot say what it offers"));

    QMutexLocker lock(&m_mutex);
    return m_offers;
}

void MemoryFileSystem::setListGate(std::shared_ptr<QSemaphore> gate)
{
    QMutexLocker lock(&m_gateMutex);
    m_listGate = std::move(gate);
}

int MemoryFileSystem::listsInProgress() const
{
    return m_listsHeld.load();
}

Result<FileEntryList> MemoryFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    checkNotOnTheDrawingThread("list");
    waitAsASlowDriveWould();

    // Held before anything else and outside the drive's own lock, so a listing
    // that is being held is a call that has not come back rather than a drive
    // that has stopped working.
    std::shared_ptr<QSemaphore> gate;
    {
        QMutexLocker lock(&m_gateMutex);
        gate = m_listGate;
    }
    if (gate) {
        ++m_listsHeld;
        gate->acquire();
        --m_listsHeld;
    }

    if (m_listDelayMs > 0) {
        // Chunked so a cancelled task does not have to wait out the full delay.
        for (int slept = 0; slept < m_listDelayMs && !cancel.isCancelled(); slept += 10)
            QThread::msleep(10);
    }
    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled"));

    QMutexLocker lock(&m_mutex);
    ++m_listCalls;

    const QString path = resolve(dir.path());
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
        entry.uri = VfsUri(dir.scheme(), dir.authority(), candidate);
        entry.isDir = it->isDir;
        entry.isSymlink = it->isSymlink;
        entry.isShortcut = it->isShortcut;
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
    checkNotOnTheDrawingThread("stat");
    waitAsASlowDriveWould();
    QMutexLocker lock(&m_mutex);
    // The stored spelling, not the one asked for: a case-insensitive volume
    // finds the file however it is typed and then reports the name it holds.
    const QString path = resolve(target.path());
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault.error();

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));

    // Answered in the address space it was asked in. A drive mounted at
    // mem://counted/ has to get mem://counted/... back, or nothing above can
    // resolve what it is handed. Only the *spelling* of the path comes from the
    // store, which is what a case-insensitive volume does.
    const VfsUri stored(target.scheme(), target.authority(), path);
    FileEntry entry;
    entry.name = stored.fileName();
    entry.uri = stored;
    entry.isDir = node->isDir;
    entry.isSymlink = node->isSymlink;
    entry.isShortcut = node->isShortcut;
    entry.isWritable = true;
    entry.size = node->isDir ? 0 : node->contents.size();
    entry.modified = node->modified;
    return entry;
}

Result<void> MemoryFileSystem::makeDirectory(const VfsUri& target)
{
    checkNotOnTheDrawingThread("makeDirectory");
    waitAsASlowDriveWould();
    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> fault = faultFor(target.path()); !fault.ok())
            return fault;
        if (m_nodes.contains(resolve(target.path())))
            return Result<void>::failure(
                VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(target.path()));
    }
    addDirectory(target.path());
    return {};
}

Result<QString> MemoryFileSystem::readLink(const VfsUri& link)
{
    checkNotOnTheDrawingThread("readLink");
    waitAsASlowDriveWould();
    QMutexLocker lock(&m_mutex);
    const QString path = resolve(link.path());
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault.error();

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such path: %1").arg(link.path()));
    if (!node->isSymlink)
        return VfsError::make(VfsError::NotALink, QStringLiteral("Not a symbolic link: %1").arg(path));
    return node->linkTarget;
}

Result<void> MemoryFileSystem::makeLink(const VfsUri& link, const QString& target)
{
    checkNotOnTheDrawingThread("makeLink");
    waitAsASlowDriveWould();
    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> fault = faultFor(link.path()); !fault.ok())
            return fault;
        if (m_nodes.contains(resolve(link.path())))
            return Result<void>::failure(
                VfsError::AlreadyExists, QStringLiteral("Already exists: %1").arg(link.path()));
    }
    addSymlink(link.path(), target);
    return {};
}

Result<void> MemoryFileSystem::remove(const VfsUri& target, bool recursive)
{
    checkNotOnTheDrawingThread("remove");
    waitAsASlowDriveWould();
    QMutexLocker lock(&m_mutex);
    const QString path = resolve(target.path());
    if (Result<void> fault = faultFor(path); !fault.ok())
        return fault;

    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("No such file: %1").arg(path));

    if (!node->isDir) {
        m_nodes.remove(path);
        touchParent(path);
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
    touchParent(path);
    return {};
}

Result<void> MemoryFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    checkNotOnTheDrawingThread("rename");
    waitAsASlowDriveWould();
    QMutexLocker lock(&m_mutex);
    const QString src = resolve(from.path());
    const QString dst = to.path();

    if (Result<void> fault = faultFor(src); !fault.ok())
        return fault;
    if (!m_nodes.contains(src))
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("No such file: %1").arg(src));

    // What is in the way, in the spelling it is stored under. It is only a
    // collision when it is a different node: on a case-insensitive volume the
    // file being renamed is the file the guard finds sitting in its own way.
    const QString occupant = resolve(dst);
    if (occupant != src && m_nodes.contains(occupant))
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

namespace {

    /// Hands its contents over a chunk at a time, pausing before each. See
    /// MemoryFileSystem::setReadThrottle() for why a paced read is worth having
    /// when a delayed one is not.
    class ThrottledReadDevice final : public QIODevice
    {
    public:
        ThrottledReadDevice(QByteArray contents, qint64 chunk, int delayMs)
            : m_contents(std::move(contents))
            , m_chunk(std::max<qint64>(1, chunk))
            , m_delayMs(std::max(0, delayMs))
        {
        }

        bool isSequential() const override { return false; }
        qint64 size() const override { return m_contents.size(); }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            const qint64 remaining = m_contents.size() - pos();
            if (remaining <= 0)
                return 0;
            if (m_delayMs > 0)
                QThread::msleep(static_cast<unsigned long>(m_delayMs));
            const qint64 taken = std::min({ maxSize, m_chunk, remaining });
            std::memcpy(data, m_contents.constData() + pos(), static_cast<size_t>(taken));
            return taken;
        }
        qint64 writeData(const char*, qint64) override { return -1; }

    private:
        QByteArray m_contents;
        qint64 m_chunk = 0;
        int m_delayMs = 0;
    };

} // namespace

Result<std::unique_ptr<QIODevice>> MemoryFileSystem::openRead(const VfsUri& target, qint64)
{
    checkNotOnTheDrawingThread("openRead");
    waitAsASlowDriveWould();
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

    if (m_throttleBytes > 0) {
        auto throttled
            = std::make_unique<ThrottledReadDevice>(node->contents, m_throttleBytes, m_throttleDelayMs);
        if (!throttled->open(QIODevice::ReadOnly))
            return VfsError::make(VfsError::IoError, QStringLiteral("Cannot open %1").arg(path));
        return Result<std::unique_ptr<QIODevice>>(std::move(throttled));
    }

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
    checkNotOnTheDrawingThread("openWrite");
    waitAsASlowDriveWould();
    // A stack-allocated MemoryFileSystem has no owning shared_ptr, and a device
    // that outlives its filesystem would be a dangling write. Down-cast from the
    // base's pointer, because the write device dereferences the concrete class.
    const std::weak_ptr<MemoryFileSystem> owner = std::static_pointer_cast<MemoryFileSystem>(sharedSelf());
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
