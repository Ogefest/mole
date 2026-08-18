#include "core/duplicates/Strategies.h"

#include "core/duplicates/ContentComparison.h"

#include <QCryptographicHash>

#ifdef MOLE_HAVE_XXHASH
#include <xxhash.h>
#endif

namespace mole {
namespace {

    /// Digests a run of bytes for the head stage.
    ///
    /// XXH3 where the build has it, which is a hundred times faster than the
    /// SHA-256 this used to use and is the difference between waiting for the
    /// processor and waiting for the disk. Nothing is being proved here -- the
    /// head is a filter, and a collision costs one extra file read at the last
    /// stage, which compares the files themselves and separates them. That is
    /// exactly why a non-cryptographic hash belongs here and nowhere else.
    ///
    /// Without xxhash the build falls back to SHA-256: slower, and every bit as
    /// correct, because a filter only has to be consistent.
    class HeadDigest
    {
    public:
#ifdef MOLE_HAVE_XXHASH
        HeadDigest()
            : m_state(XXH3_createState())
        {
            XXH3_128bits_reset(m_state);
        }
        ~HeadDigest() { XXH3_freeState(m_state); }
        void add(QByteArrayView bytes) { XXH3_128bits_update(m_state, bytes.data(), bytes.size()); }
        QString result() const
        {
            const XXH128_hash_t digest = XXH3_128bits_digest(m_state);
            return QStringLiteral("%1%2")
                .arg(digest.high64, 16, 16, QLatin1Char('0'))
                .arg(digest.low64, 16, 16, QLatin1Char('0'));
        }

    private:
        XXH3_state_t* m_state = nullptr;
#else
        void add(QByteArrayView bytes) { m_hash.addData(bytes); }
        QString result() const { return QString::fromLatin1(m_hash.result().toHex()); }

    private:
        QCryptographicHash m_hash { QCryptographicHash::Sha256 };
#endif
    };

    /// Hashes at most `limit` bytes of a file. Returns an empty key when the file
    /// cannot be read -- an unreadable file is not a match for every other
    /// unreadable file.
    QString hashOf(const FileEntry& entry, IFileSystem* fileSystem, qint64 limit, const CancelToken& cancel)
    {
        if (!fileSystem)
            return {};

        Result<std::unique_ptr<QIODevice>> opened = fileSystem->openRead(entry.uri);
        if (!opened.ok())
            return {};
        std::unique_ptr<QIODevice> device = std::move(opened.value());
        if (!device)
            return {};

        HeadDigest hash;
        constexpr qint64 kChunk = 256 * 1024;
        qint64 read = 0;

        while (!device->atEnd()) {
            if (cancel.isCancelled())
                return {};
            const qint64 wanted = limit < 0 ? kChunk : std::min(kChunk, limit - read);
            if (wanted <= 0)
                break;
            const QByteArray chunk = device->read(wanted);
            if (chunk.isEmpty())
                break;
            hash.add(chunk);
            read += chunk.size();
        }

        // The size goes into the key as well, so a short file cannot collide with
        // the first megabyte of a long one.
        hash.add(QByteArray::number(entry.size));
        return hash.result();
    }

} // namespace

QList<QList<FileEntry>> IDuplicateStrategy::compare(
    int, const QList<FileEntry>&, const DriveLookup&, const CancelToken&) const
{
    // Only ever reached by a strategy that said one of its stages compares and
    // then did not implement it.
    Q_ASSERT_X(false, "IDuplicateStrategy::compare", "stage compares content but compare() is missing");
    return {};
}

QString SameSizeStrategy::keyFor(int, const FileEntry& entry, IFileSystem*, const CancelToken&) const
{
    // A zero-byte file is not usefully a duplicate of every other empty file,
    // and grouping thousands of them buries the results that matter.
    return entry.size > 0 ? QString::number(entry.size) : QString();
}

QString SameNameStrategy::keyFor(int, const FileEntry& entry, IFileSystem*, const CancelToken&) const
{
    return entry.name.toLower();
}

QString SameNameAndSizeStrategy::keyFor(
    int stage, const FileEntry& entry, IFileSystem*, const CancelToken&) const
{
    if (stage == 0)
        return entry.size > 0 ? QString::number(entry.size) : QString();
    return entry.name.toLower();
}

QString SameContentStrategy::keyFor(
    int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const
{
    switch (stage) {
    case 0:
        return entry.size > 0 ? QString::number(entry.size) : QString();
    case 1:
        // Skipped for a file that fits entirely in the head: reading it twice
        // would double the work for the smallest files, which are the most
        // numerous. With the head at a megabyte that is most files in most
        // trees, and they go straight to the comparison having cost one read
        // rather than two.
        if (entry.size <= kHeadBytes)
            return QStringLiteral("small");
        return hashOf(entry, fileSystem, kHeadBytes, cancel);
    default:
        Q_ASSERT_X(false, "SameContentStrategy::keyFor", "the last stage compares, it does not key");
        return {};
    }
}

QList<QList<FileEntry>> SameContentStrategy::compare(
    int, const QList<FileEntry>& bucket, const DriveLookup& driveFor, const CancelToken& cancel) const
{
    return partitionByContents(bucket, driveFor, cancel);
}

std::vector<std::unique_ptr<IDuplicateStrategy>> IDuplicateStrategy::all()
{
    std::vector<std::unique_ptr<IDuplicateStrategy>> out;
    out.push_back(std::make_unique<SameContentStrategy>());
    out.push_back(std::make_unique<SameNameAndSizeStrategy>());
    out.push_back(std::make_unique<SameNameStrategy>());
    out.push_back(std::make_unique<SameSizeStrategy>());
    return out;
}

} // namespace mole
