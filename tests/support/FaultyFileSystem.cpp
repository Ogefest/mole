#include "support/FaultyFileSystem.h"

#include <QMutex>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace mole::test {

/// What was declared, plus the little state the open streams share.
///
/// A stream takes its own copy of the faults that apply to it when it is opened,
/// so two concurrent transfers each get the whole fault rather than racing for
/// it -- ten copies of one file should each drop at 30%, not one of them.
struct FaultyFileSystem::Policy
{
    enum class Side { Read, Write };

    /// What the fault does to the stream. Any action attached to it has already
    /// run by the time the effect is applied.
    enum class Effect { None, Fail, Stall, Short, EndsEarly };

    struct Fault
    {
        Side side = Side::Read;
        qint64 offset = 0;
        QString path; ///< empty: every file opened through this wrapper
        std::function<void(const VfsUri&)> action;
        Effect effect = Effect::None;
        qint64 amount = 0; ///< the short chunk, or the file's new size
        VfsError error;
        bool fired = false; ///< per stream, because each open takes a copy
    };

    QList<Fault> forStream(Side side, const VfsUri& target) const
    {
        QMutexLocker lock(&mutex);
        QList<Fault> mine;
        for (const Fault& fault : faults) {
            if (fault.side == side && (fault.path.isEmpty() || fault.path == target.path()))
                mine.append(fault);
        }
        return mine;
    }

    int keepEveryFor(const VfsUri& target) const
    {
        QMutexLocker lock(&mutex);
        for (const auto& [path, every] : keepEvery) {
            if (path.isEmpty() || path == target.path())
                return every;
        }
        return 1;
    }

    /// Blocks until release(). Called on a worker thread, which is the point:
    /// the transfer really is stopped mid-file while the test does something.
    void stall()
    {
        QMutexLocker lock(&mutex);
        ++stalledStreams;
        while (!released)
            wake.wait(&mutex);
        --stalledStreams;
    }

    /// The first refusal that covers this path, if there is one.
    std::optional<VfsError> refusedRemoval(const VfsUri& target) const
    {
        QMutexLocker lock(&mutex);
        for (const auto& [path, error] : removeRefusals) {
            if (path.isEmpty() || path == target.path() || target.path().startsWith(path + QLatin1Char('/')))
                return error;
        }
        return std::nullopt;
    }

    mutable QMutex mutex;
    QWaitCondition wake;
    QList<Fault> faults;
    QList<QPair<QString, VfsError>> removeRefusals;
    QList<QPair<QString, int>> keepEvery;
    int stalledStreams = 0;
    bool released = false;
    std::atomic_bool revoked { false };
    /// What went through, for a test that is about how much was read rather than
    /// about what came back. Atomic because the streams run on worker threads.
    /// One entry per stream opened, holding what it delivered. Guarded by the
    /// mutex rather than atomic, because a list is not one word.
    QList<qint64> readSizes;

    int openedForRead()
    {
        QMutexLocker lock(&mutex);
        readSizes.append(0);
        return static_cast<int>(readSizes.size()) - 1;
    }

    void delivered(int stream, qint64 bytes)
    {
        QMutexLocker lock(&mutex);
        if (stream >= 0 && stream < readSizes.size())
            readSizes[stream] += bytes;
    }
    qint64 listingSizeDelta = 0;
    std::atomic_bool noRandomAccess { false };
    bool failOnClose = false;
    QString closeMessage;
};

namespace {

    using Policy = FaultyFileSystem::Policy;

    VfsError revokedError()
    {
        return VfsError::make(VfsError::AccessDenied, QStringLiteral("permission was withdrawn"));
    }

    /// Counts the bytes handed over and acts when the count arrives at an offset.
    class FaultyReadDevice final : public QIODevice
    {
    public:
        FaultyReadDevice(std::unique_ptr<QIODevice> inner, std::shared_ptr<Policy> policy,
            QList<Policy::Fault> faults, VfsUri target, int stream)
            : m_inner(std::move(inner))
            , m_policy(std::move(policy))
            , m_faults(std::move(faults))
            , m_target(std::move(target))
            , m_stream(stream)
        {
        }

        bool open(OpenMode mode) override { return QIODevice::open(mode | QIODevice::Unbuffered); }

        // Forwarded, or a caller that seeks reads from wherever the inner stream
        // happened to be -- which looks like a reader ignoring an offset rather
        // than like a wrapper swallowing one.
        bool isSequential() const override { return !m_inner || m_inner->isSequential(); }
        bool seek(qint64 position) override
        {
            if (isSequential() || !QIODevice::seek(position))
                return false;
            return m_inner->seek(position);
        }
        void close() override
        {
            if (m_inner)
                m_inner->close();
            QIODevice::close();
        }
        qint64 size() const override { return m_inner ? m_inner->size() : 0; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            // A stream that has failed stays failed. A backend does not recover
            // a connection it has already lost, and a caller that keeps reading
            // must keep being told no.
            if (m_failed)
                return -1;

            for (Policy::Fault& fault : m_faults) {
                if (fault.fired || m_delivered < fault.offset)
                    continue;
                fault.fired = true;
                if (fault.action)
                    fault.action(m_target);

                switch (fault.effect) {
                case Policy::Effect::Fail:
                    m_failed = true;
                    setErrorString(fault.error.message);
                    return -1;
                case Policy::Effect::Stall:
                    m_policy->stall();
                    break;
                case Policy::Effect::Short:
                    m_shortChunk = std::max<qint64>(1, fault.amount);
                    break;
                case Policy::Effect::EndsEarly:
                    m_endsAt = fault.amount;
                    break;
                case Policy::Effect::None:
                    break;
                }
            }

            if (m_endsAt >= 0 && m_delivered >= m_endsAt)
                return 0;

            // Clamped so the next fault fires on its own byte rather than on
            // whatever boundary the caller's chunk size happens to fall on.
            qint64 permitted = maxSize;
            for (const Policy::Fault& fault : m_faults) {
                if (!fault.fired && fault.offset > m_delivered)
                    permitted = std::min(permitted, fault.offset - m_delivered);
            }
            if (m_shortChunk > 0) {
                permitted = std::min(permitted, m_shortChunk);
                m_shortChunk = 0;
            }
            if (m_endsAt >= 0)
                permitted = std::min(permitted, m_endsAt - m_delivered);

            const qint64 got = m_inner->read(data, permitted);
            if (got > 0) {
                m_delivered += got;
                m_policy->delivered(m_stream, got);
            }
            return got;
        }

        qint64 writeData(const char*, qint64) override { return -1; }

    private:
        std::unique_ptr<QIODevice> m_inner;
        std::shared_ptr<Policy> m_policy;
        QList<Policy::Fault> m_faults;
        VfsUri m_target;
        int m_stream = -1;
        qint64 m_delivered = 0;
        qint64 m_shortChunk = 0;
        qint64 m_endsAt = -1;
        bool m_failed = false;
    };

    /// The same, for the side that is being written to.
    class FaultyWriteDevice final : public QIODevice, public ICommitsOnClose
    {
    public:
        FaultyWriteDevice(std::unique_ptr<QIODevice> inner, std::shared_ptr<Policy> policy,
            QList<Policy::Fault> faults, VfsUri target, int keepEvery, VfsError closeError)
            : m_inner(std::move(inner))
            , m_policy(std::move(policy))
            , m_faults(std::move(faults))
            , m_target(std::move(target))
            , m_keepEvery(keepEvery)
            , m_closeError(std::move(closeError))
        {
        }

        bool open(OpenMode mode) override { return QIODevice::open(mode | QIODevice::Unbuffered); }

        // Forwarded, or a caller that seeks reads from wherever the inner stream
        // happened to be -- which looks like a reader ignoring an offset rather
        // than like a wrapper swallowing one.
        bool isSequential() const override { return !m_inner || m_inner->isSequential(); }
        bool seek(qint64 position) override
        {
            if (isSequential() || !QIODevice::seek(position))
                return false;
            return m_inner->seek(position);
        }
        void close() override
        {
            if (m_inner)
                m_inner->close();
            QIODevice::close();
        }

        VfsError commitError() const override
        {
            if (m_closeError.isError())
                return m_closeError;
            if (auto* buffered = dynamic_cast<ICommitsOnClose*>(m_inner.get()))
                return buffered->commitError();
            return VfsError::ok();
        }

    protected:
        qint64 readData(char*, qint64) override { return -1; }

        qint64 writeData(const char* data, qint64 size) override
        {
            qint64 done = 0;
            while (done < size) {
                for (Policy::Fault& fault : m_faults) {
                    if (fault.fired || m_written < fault.offset)
                        continue;
                    fault.fired = true;
                    if (fault.action)
                        fault.action(m_target);

                    if (fault.effect == Policy::Effect::Fail) {
                        setErrorString(fault.error.message);
                        // What was accepted before the offset stays accepted: a
                        // disk that fills up keeps the bytes it already took,
                        // and the caller is told it wrote fewer than it asked.
                        return done > 0 ? done : -1;
                    }
                    if (fault.effect == Policy::Effect::Stall)
                        m_policy->stall();
                }

                qint64 permitted = size - done;
                for (const Policy::Fault& fault : m_faults) {
                    if (!fault.fired && fault.offset > m_written)
                        permitted = std::min(permitted, fault.offset - m_written);
                }

                if (!pass(data + done, permitted))
                    return -1;
                m_written += permitted;
                done += permitted;
            }
            return done;
        }

    private:
        /// Hands the segment on to the drive underneath, keeping only every Nth
        /// byte when the drive is one that loses them, and reporting nothing
        /// about it -- that is the whole fault.
        bool pass(const char* data, qint64 size)
        {
            if (!m_inner)
                return true; // the payload goes nowhere, which is what a failed commit means
            if (m_keepEvery <= 1)
                return m_inner->write(data, size) == size;

            QByteArray kept;
            for (qint64 i = 0; i < size; ++i) {
                if ((m_written + i) % m_keepEvery == 0)
                    kept.append(data[i]);
            }
            return m_inner->write(kept) == kept.size();
        }

        std::unique_ptr<QIODevice> m_inner;
        std::shared_ptr<Policy> m_policy;
        QList<Policy::Fault> m_faults;
        VfsUri m_target;
        qint64 m_written = 0;
        int m_keepEvery = 1;
        VfsError m_closeError;
    };

} // namespace

FaultyFileSystem::FaultyFileSystem(FileSystemPtr inner)
    : m_inner(std::move(inner))
    , m_policy(std::make_shared<Policy>())
{
}

FaultyFileSystem::~FaultyFileSystem()
{
    // A stream left stalled would hold its worker thread for ever, and a test
    // that failed before its release() should fail, not hang.
    release();
}

// ---- declaring faults ------------------------------------------------------

namespace {

    Policy::Fault readFault(qint64 offset, const QString& path, Policy::Effect effect)
    {
        Policy::Fault fault;
        fault.side = Policy::Side::Read;
        fault.offset = offset;
        fault.path = path;
        fault.effect = effect;
        return fault;
    }

    Policy::Fault writeFault(qint64 offset, const QString& path, Policy::Effect effect)
    {
        Policy::Fault fault;
        fault.side = Policy::Side::Write;
        fault.offset = offset;
        fault.path = path;
        fault.effect = effect;
        return fault;
    }

} // namespace

FaultyFileSystem& FaultyFileSystem::readFailsAt(
    qint64 offset, const QString& path, VfsError::Code code, const QString& message)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::Fail);
    fault.error = VfsError::make(code, message);
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::readGoesShortAt(qint64 offset, qint64 chunk, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::Short);
    fault.amount = chunk;
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::readStallsAt(qint64 offset, const QString& path)
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(readFault(offset, path, Policy::Effect::Stall));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::whenReadReaches(
    qint64 offset, std::function<void()> action, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::None);
    fault.action = [action = std::move(action)](const VfsUri&) { action(); };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::writeFailsAt(
    qint64 offset, const QString& path, VfsError::Code code, const QString& message)
{
    Policy::Fault fault = writeFault(offset, path, Policy::Effect::Fail);
    fault.error = VfsError::make(code, message);
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::destinationFillsAt(qint64 offset, const QString& path)
{
    return writeFailsAt(offset, path, VfsError::IoError, QStringLiteral("no space left on the destination"));
}

FaultyFileSystem& FaultyFileSystem::writeKeepsEveryNth(int keepEvery, const QString& path)
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->keepEvery.append({ path, std::max(1, keepEvery) });
    return *this;
}

FaultyFileSystem& FaultyFileSystem::writeFailsOnClose(const QString& message)
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->failOnClose = true;
    m_policy->closeMessage = message;
    return *this;
}

FaultyFileSystem& FaultyFileSystem::whenWriteReaches(
    qint64 offset, std::function<void()> action, const QString& path)
{
    Policy::Fault fault = writeFault(offset, path, Policy::Effect::None);
    fault.action = [action = std::move(action)](const VfsUri&) { action(); };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::fileChangesSizeAt(qint64 offset, qint64 newSize, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::EndsEarly);
    fault.amount = newSize;
    // Written through the backend itself rather than into a fixture, so the
    // same fault works over memory, local disk and anything else.
    fault.action = [inner = m_inner, newSize](const VfsUri& target) {
        QByteArray contents;
        if (Result<std::unique_ptr<QIODevice>> reader = inner->openRead(target); reader.ok())
            contents = reader.value()->readAll();
        if (contents.size() > newSize)
            contents.truncate(newSize);
        while (contents.size() < newSize)
            contents.append('x');

        if (Result<std::unique_ptr<QIODevice>> writer = inner->openWrite(target, newSize); writer.ok()) {
            writer.value()->write(contents);
            closeAndReport(*writer.value());
        }
    };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::fileVanishesAt(qint64 offset, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::Fail);
    fault.error
        = VfsError::make(VfsError::NotFound, QStringLiteral("the file went away while it was being read"));
    fault.action = [inner = m_inner](const VfsUri& target) { inner->remove(target, false); };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::fileIsRenamedAt(
    qint64 offset, const QString& newName, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::Fail);
    fault.error
        = VfsError::make(VfsError::NotFound, QStringLiteral("the file was renamed while it was being read"));
    fault.action = [inner = m_inner, newName](
                       const VfsUri& target) { inner->rename(target, target.parent().child(newName)); };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::accessRevokedAt(qint64 offset, const QString& path)
{
    Policy::Fault fault = readFault(offset, path, Policy::Effect::Fail);
    fault.error = revokedError();
    // Weak, and this is the one that matters: the policy owns the fault, the
    // fault owns this lambda, and a shared_ptr here would close the ring --
    // every drive built with a revocation would then live until the process
    // ended, along with everything it wrapped.
    fault.action = [policy = std::weak_ptr<Policy>(m_policy)](const VfsUri&) {
        if (const std::shared_ptr<Policy> alive = policy.lock())
            alive->revoked.store(true);
    };
    QMutexLocker lock(&m_policy->mutex);
    m_policy->faults.append(std::move(fault));
    return *this;
}

FaultyFileSystem& FaultyFileSystem::listingOverstatesSizeBy(qint64 bytes)
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->listingSizeDelta = bytes;
    return *this;
}

FaultyFileSystem& FaultyFileSystem::cannotSeek()
{
    m_policy->noRandomAccess.store(true);
    return *this;
}

int FaultyFileSystem::openReadCount() const
{
    QMutexLocker lock(&m_policy->mutex);
    return static_cast<int>(m_policy->readSizes.size());
}

qint64 FaultyFileSystem::bytesRead() const
{
    QMutexLocker lock(&m_policy->mutex);
    qint64 total = 0;
    for (const qint64 bytes : m_policy->readSizes)
        total += bytes;
    return total;
}

QList<qint64> FaultyFileSystem::readSizes() const
{
    QMutexLocker lock(&m_policy->mutex);
    return m_policy->readSizes;
}

bool FaultyFileSystem::isStalled() const
{
    QMutexLocker lock(&m_policy->mutex);
    return m_policy->stalledStreams > 0;
}

void FaultyFileSystem::release()
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->released = true;
    m_policy->wake.wakeAll();
}

// ---- the drive itself ------------------------------------------------------

QString FaultyFileSystem::scheme() const
{
    return m_inner->scheme();
}

VfsCapabilities FaultyFileSystem::capabilities() const
{
    VfsCapabilities capabilities = m_inner->capabilities();
    if (m_policy->noRandomAccess.load())
        capabilities.setFlag(VfsCapability::RandomAccessRead, false);
    return capabilities;
}

Result<FileEntryList> FaultyFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    if (m_policy->revoked.load())
        return revokedError();

    Result<FileEntryList> listed = m_inner->list(dir, cancel);
    qint64 delta = 0;
    {
        QMutexLocker lock(&m_policy->mutex);
        delta = m_policy->listingSizeDelta;
    }
    if (!listed.ok() || delta == 0)
        return listed;

    FileEntryList entries = listed.value();
    for (FileEntry& entry : entries) {
        if (!entry.isDir)
            entry.size += delta;
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntry> FaultyFileSystem::stat(const VfsUri& target)
{
    if (m_policy->revoked.load())
        return revokedError();

    Result<FileEntry> entry = m_inner->stat(target);
    qint64 delta = 0;
    {
        QMutexLocker lock(&m_policy->mutex);
        delta = m_policy->listingSizeDelta;
    }
    if (!entry.ok() || delta == 0 || entry.value().isDir)
        return entry;

    FileEntry lying = entry.value();
    lying.size += delta;
    return Result<FileEntry>(lying);
}

Result<void> FaultyFileSystem::makeDirectory(const VfsUri& target)
{
    if (m_policy->revoked.load())
        return revokedError();
    return m_inner->makeDirectory(target);
}

Result<void> FaultyFileSystem::remove(const VfsUri& target, bool recursive)
{
    if (m_policy->revoked.load())
        return revokedError();
    // A refusal covers the path and everything under it, which is what a
    // directory nobody may write to actually does to a recursive delete.
    if (const std::optional<VfsError> refused = m_policy->refusedRemoval(target))
        return Result<void>(*refused);
    return m_inner->remove(target, recursive);
}

FaultyFileSystem& FaultyFileSystem::removeFails(
    const QString& path, VfsError::Code code, const QString& message)
{
    QMutexLocker lock(&m_policy->mutex);
    m_policy->removeRefusals.append({ path, VfsError::make(code, message) });
    return *this;
}

Result<void> FaultyFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    if (m_policy->revoked.load())
        return revokedError();
    return m_inner->rename(from, to);
}

Result<std::unique_ptr<QIODevice>> FaultyFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    if (m_policy->revoked.load())
        return revokedError();

    Result<std::unique_ptr<QIODevice>> inner = m_inner->openRead(target, expectedSize);
    if (!inner.ok())
        return inner;
    auto device = std::make_unique<FaultyReadDevice>(std::move(inner.value()), m_policy,
        m_policy->forStream(Policy::Side::Read, target), target, m_policy->openedForRead());
    device->open(QIODevice::ReadOnly);
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(device.release()));
}

Result<std::unique_ptr<QIODevice>> FaultyFileSystem::openWrite(const VfsUri& target, qint64 expectedSize)
{
    if (m_policy->revoked.load())
        return revokedError();

    bool failOnClose = false;
    QString closeMessage;
    {
        QMutexLocker lock(&m_policy->mutex);
        failOnClose = m_policy->failOnClose;
        closeMessage = m_policy->closeMessage;
    }

    // A drive that fails when it commits never had the payload: it staged it and
    // the send failed, so nothing is opened underneath and nothing lands.
    std::unique_ptr<QIODevice> innerDevice;
    if (!failOnClose) {
        Result<std::unique_ptr<QIODevice>> inner = m_inner->openWrite(target, expectedSize);
        if (!inner.ok())
            return inner;
        innerDevice = std::move(inner.value());
    }

    auto device = std::make_unique<FaultyWriteDevice>(std::move(innerDevice), m_policy,
        m_policy->forStream(Policy::Side::Write, target), target, m_policy->keepEveryFor(target),
        failOnClose ? VfsError::make(VfsError::NetworkError, closeMessage) : VfsError::ok());
    device->open(QIODevice::WriteOnly);
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(device.release()));
}

Result<SpaceInfo> FaultyFileSystem::space(const VfsUri& target)
{
    if (m_policy->revoked.load())
        return revokedError();
    return m_inner->space(target);
}

Result<AccessInfo> FaultyFileSystem::access(const VfsUri& target)
{
    if (m_policy->revoked.load())
        return revokedError();
    return m_inner->access(target);
}

Result<FileEntryList> FaultyFileSystem::search(
    const VfsUri& root, const QString& pattern, const CancelToken& cancel)
{
    if (m_policy->revoked.load())
        return revokedError();
    return m_inner->search(root, pattern, cancel);
}

} // namespace mole::test
