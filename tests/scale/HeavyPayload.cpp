#include "scale/HeavyPayload.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>
#include <QStandardPaths>

#include <chrono>

namespace mole::test {
namespace {

    /// One megabyte of bytes that are not compressible and not all the same, so
    /// a backend that helpfully compresses, deduplicates or sparsifies cannot
    /// make the measurement mean something else.
    const QByteArray& baseBlock()
    {
        static const QByteArray base = [] {
            QByteArray out(HeavyPayload::kBlockSize, Qt::Uninitialized);
            quint64 state = 0x0DDB1A5E5BAD5EEDull;
            for (qsizetype i = 0; i < out.size(); ++i) {
                // splitmix64, one byte of each step. Deterministic across
                // machines and compilers, which a std:: generator is not.
                state += 0x9E3779B97F4A7C15ull;
                quint64 z = state;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                out[i] = char((z ^ (z >> 31)) & 0xff);
            }
            return out;
        }();
        return base;
    }

    void stamp(QByteArray& block, qint64 index)
    {
        for (int i = 0; i < 8; ++i)
            block[i] = char((quint64(index) >> (8 * i)) & 0xff);
    }

    qint64 residentBytes()
    {
        QFile statm(QStringLiteral("/proc/self/statm"));
        if (!statm.open(QIODevice::ReadOnly))
            return 0;
        const QByteArrayList fields = statm.readLine().simplified().split(' ');
        if (fields.size() < 2)
            return 0;
        return fields.at(1).toLongLong() * qint64(sysconf(_SC_PAGESIZE));
    }

    /// Every entry directly in the temporary directory, and inside any directory
    /// that appeared there since the baseline. One level of recursion, because
    /// staging puts its files in a directory of its own and nothing sensible
    /// buries them deeper.
    QSet<QString> tempEntries()
    {
        QSet<QString> names;
        const QString root = QDir::tempPath();
        QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        while (it.hasNext())
            names.insert(it.next());
        return names;
    }

} // namespace

void HeavyPayload::block(QByteArray& out, qint64 blockIndex)
{
    out = baseBlock();
    stamp(out, blockIndex);
}

bool HeavyPayload::writeTo(QIODevice& device, qint64 bytes, QString* errorOut)
{
    QByteArray buffer;
    qint64 written = 0;
    for (qint64 index = 0; written < bytes; ++index) {
        block(buffer, index);
        const qint64 wanted = qMin<qint64>(kBlockSize, bytes - written);
        const qint64 put = device.write(buffer.constData(), wanted);
        if (put != wanted) {
            if (errorOut) {
                *errorOut = QStringLiteral("the payload stopped at %1 of %2 bytes: %3")
                                .arg(written + qMax<qint64>(put, 0))
                                .arg(bytes)
                                .arg(device.errorString());
            }
            return false;
        }
        written += put;
    }
    return true;
}

bool HeavyPayload::verify(QIODevice& device, qint64 expectedBytes, QString* errorOut)
{
    QByteArray expected;
    QByteArray got(kBlockSize, Qt::Uninitialized);
    qint64 read = 0;

    for (qint64 index = 0;; ++index) {
        const qint64 wanted = qMin<qint64>(kBlockSize, expectedBytes - read);
        if (wanted <= 0)
            break;

        qint64 filled = 0;
        while (filled < wanted) {
            const qint64 chunk = device.read(got.data() + filled, wanted - filled);
            if (chunk < 0) {
                if (errorOut) {
                    *errorOut = QStringLiteral("the copy could not be read past %1 bytes: %2")
                                    .arg(read + filled)
                                    .arg(device.errorString());
                }
                return false;
            }
            if (chunk == 0)
                break;
            filled += chunk;
        }

        if (filled < wanted) {
            if (errorOut) {
                *errorOut = QStringLiteral("the copy ended at %1 bytes, %2 short")
                                .arg(read + filled)
                                .arg(expectedBytes - read - filled);
            }
            return false;
        }

        block(expected, index);
        if (std::memcmp(expected.constData(), got.constData(), size_t(wanted)) != 0) {
            qint64 offset = 0;
            while (offset < wanted && expected.at(offset) == got.at(offset))
                ++offset;
            if (errorOut) {
                *errorOut = QStringLiteral("the copy differs from byte %1 (block %2, offset %3 in it)")
                                .arg(read + offset)
                                .arg(index)
                                .arg(offset);
            }
            return false;
        }
        read += filled;
    }

    // Anything after the expected length is as wrong as anything missing.
    char extra = 0;
    if (device.read(&extra, 1) > 0) {
        if (errorOut)
            *errorOut
                = QStringLiteral("the copy is longer than the %1 bytes that were sent").arg(expectedBytes);
        return false;
    }
    return true;
}

ResourceWatch::ResourceWatch()
    : m_baselineRss(residentBytes())
    , m_baselineFds(openDescriptors())
    , m_tempPath(QDir::tempPath())
{
    const QSet<QString> before = tempEntries();
    m_baselineEntries = QStringList(before.values()).join(QLatin1Char('\n')).toUtf8();
    m_sampler = std::thread([this] { sample(); });
}

ResourceWatch::~ResourceWatch()
{
    stop();
}

void ResourceWatch::stop()
{
    if (!m_running.exchange(false))
        return;
    if (m_sampler.joinable())
        m_sampler.join();
}

void ResourceWatch::sample()
{
    const QSet<QString> baseline = [this] {
        QSet<QString> out;
        for (const QString& name : QString::fromUtf8(m_baselineEntries).split(QLatin1Char('\n')))
            out.insert(name);
        return out;
    }();

    while (m_running.load()) {
        qint64 scratch = 0;
        QString largest;
        qint64 largestBytes = 0;
        QDirIterator it(m_tempPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        while (it.hasNext()) {
            const QString path = it.next();
            if (baseline.contains(path))
                continue;
            const QFileInfo info = it.fileInfo();
            qint64 here = 0;
            if (info.isDir()) {
                QDirIterator inner(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
                while (inner.hasNext()) {
                    inner.next();
                    here += inner.fileInfo().size();
                }
            } else {
                here = info.size();
            }
            scratch += here;
            if (here > largestBytes) {
                largestBytes = here;
                largest = path;
            }
        }
        if (scratch > m_peakScratch.load()) {
            m_peakScratch.store(scratch);
            const std::lock_guard<std::mutex> guard(m_namesGuard);
            m_largestEntry = largest;
        }

        const qint64 growth = residentBytes() - m_baselineRss;
        if (growth > m_peakRss.load())
            m_peakRss.store(growth);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

QString ResourceWatch::largestScratchEntry() const
{
    const std::lock_guard<std::mutex> guard(m_namesGuard);
    return m_largestEntry;
}

int ResourceWatch::openDescriptors()
{
    QDir fds(QStringLiteral("/proc/self/fd"));
    return int(fds.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System).size());
}

QString ResourceWatch::summary() const
{
    return QStringLiteral("peak scratch %1 KiB, peak RSS growth %2 KiB, descriptors %3 -> %4")
        .arg(m_peakScratch.load() / 1024)
        .arg(m_peakRss.load() / 1024)
        .arg(m_baselineFds)
        .arg(openDescriptors());
}

} // namespace mole::test
