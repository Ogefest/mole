#include "core/duplicates/Strategies.h"

#include <QCryptographicHash>

namespace mole {
namespace {

    /// Hashes at most `limit` bytes, or the whole file when `limit` is negative.
    /// Returns an empty key when the file cannot be read -- an unreadable file is
    /// not a match for every other unreadable file.
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

        QCryptographicHash hash(QCryptographicHash::Sha256);
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
            hash.addData(chunk);
            read += chunk.size();
        }

        // The size goes into the key as well, so a short file cannot collide with
        // the first 16 kB of a long one.
        hash.addData(QByteArray::number(entry.size));
        return QString::fromLatin1(hash.result().toHex());
    }

} // namespace

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
        // Skipped for a file that fits entirely in the head: hashing it twice
        // would double the work for the smallest files, which are the most
        // numerous.
        if (entry.size <= kHeadBytes)
            return QStringLiteral("small");
        return hashOf(entry, fileSystem, kHeadBytes, cancel);
    default:
        return hashOf(entry, fileSystem, -1, cancel);
    }
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
