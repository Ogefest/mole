#include "plugins/archive/CompressTask.h"

#include "core/vfs/DirectoryWalker.h"

#include <QFileInfo>
#include <QIODevice>
#include <QLocale>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>

namespace mole {
namespace {

    /// Read in chunks, so a sixty-gigabyte file costs a chunk of memory and not
    /// sixty gigabytes of it.
    constexpr qint64 kChunkBytes = 256 * 1024;

    /// libarchive writes through this rather than to a path of its own, so the
    /// archive can land on any drive the application can write to.
    struct WriteContext
    {
        QIODevice* device = nullptr;
        QString error;
    };

    la_ssize_t writeToDevice(archive*, void* opaque, const void* buffer, size_t length)
    {
        auto* context = static_cast<WriteContext*>(opaque);
        if (!context || !context->device)
            return -1;
        const qint64 written
            = context->device->write(static_cast<const char*>(buffer), static_cast<qint64>(length));
        if (written < 0) {
            context->error = context->device->errorString();
            return -1;
        }
        return static_cast<la_ssize_t>(written);
    }

} // namespace

QStringList CompressTask::formatNames()
{
    return { QStringLiteral("zip"), QStringLiteral("tar.gz"), QStringLiteral("tar.xz"), QStringLiteral("7z"),
        QStringLiteral("xz") };
}

CompressTask::Format CompressTask::formatFromName(const QString& name)
{
    const QString lower = name.trimmed().toLower();
    if (lower == QLatin1String("tar.gz") || lower == QLatin1String("tgz"))
        return Format::TarGz;
    if (lower == QLatin1String("tar.xz") || lower == QLatin1String("txz"))
        return Format::TarXz;
    if (lower == QLatin1String("7z") || lower == QLatin1String("7zip"))
        return Format::SevenZip;
    // After tar.xz, or "tar.xz" would be read as a bare xz stream.
    if (lower == QLatin1String("xz"))
        return Format::Xz;
    // Zip for anything unrecognised: it is the one anyone can open anywhere, which
    // makes it the right thing to fall back to as well as the right default.
    return Format::Zip;
}

QString CompressTask::suffixFor(Format format)
{
    switch (format) {
    case Format::TarGz:
        return QStringLiteral(".tar.gz");
    case Format::TarXz:
        return QStringLiteral(".tar.xz");
    case Format::SevenZip:
        return QStringLiteral(".7z");
    case Format::Xz:
        return QStringLiteral(".xz");
    case Format::Zip:
        break;
    }
    return QStringLiteral(".zip");
}

bool CompressTask::formatSupportsPassword(Format format)
{
    // Zip only, and measured rather than assumed: this libarchive rejects
    // "7zip:encryption" as an undefined option. Worse, it accepts a passphrase for
    // 7z regardless and the written file contains no plain text -- because LZMA2
    // compressed it, not because anything encrypted it. A test looking only for the
    // plain bytes would call that success, which is why the answer is a fact about
    // the format rather than an inference from the output.
    return format == Format::Zip;
}

bool CompressTask::takesOneFileOnly(Format format)
{
    return format == Format::Xz;
}

QString CompressTask::nameWithSuffix(const QString& name, Format format)
{
    QString base = name.trimmed();

    // Longest first, or ".tar.gz" would lose only the ".gz" and leave "notes.tar".
    QStringList known;
    for (const QString& formatName : formatNames())
        known.append(QLatin1Char('.') + formatName);
    std::sort(
        known.begin(), known.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });

    for (const QString& suffix : known) {
        if (base.endsWith(suffix, Qt::CaseInsensitive)) {
            base.chop(suffix.size());
            break;
        }
    }

    if (base.isEmpty())
        return {};
    return base + suffixFor(format);
}

CompressTask::CompressTask(Request request, QObject* parent)
    : Task(request.sources.size() == 1 ? QStringLiteral("Compress %1").arg(request.sources.first().fileName())
                                       : QStringLiteral("Compress %1 items").arg(request.sources.size()),
          parent)
    , m_request(std::move(request))
{
}

bool CompressTask::plan(QList<Item>& items)
{
    for (const VfsUri& source : m_request.sources) {
        if (isCancelRequested())
            return false;

        Result<FileEntry> stat = m_request.sourceFileSystem->stat(source);
        if (!stat.ok()) {
            m_failures.append(QStringLiteral("%1: %2").arg(source.fileName(), stat.error().message));
            continue;
        }

        const FileEntry entry = stat.value();
        if (!entry.isDir) {
            items.append(Item { source, entry.name, false, entry.size });
            continue;
        }

        // A folder goes in with what is inside it, and the names inside the archive
        // are relative to the folder itself rather than to the drive -- unpacking
        // should give back the folder, not a chain of parent directories.
        items.append(Item { source, entry.name + QLatin1Char('/'), true, 0 });

        const QString base = source.path();
        DirectoryWalker walker(m_request.sourceFileSystem);
        walker.walk(source, cancelToken(), [&](const FileEntry& found, int) {
            QString relative = found.uri.path().mid(base.size());
            if (relative.startsWith(QLatin1Char('/')))
                relative.remove(0, 1);
            const QString inArchive = entry.name + QLatin1Char('/') + relative;
            items.append(Item { found.uri, found.isDir ? inArchive + QLatin1Char('/') : inArchive,
                found.isDir, found.isDir ? 0 : found.size });
            return DirectoryWalker::Action::Continue;
        });
    }
    return !items.isEmpty();
}

void CompressTask::discardPartialArchive()
{
    if (m_request.targetFileSystem)
        m_request.targetFileSystem->remove(m_request.target, false);
}

void CompressTask::run()
{
    if (!m_request.sourceFileSystem || !m_request.targetFileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted for this")));
        return;
    }

    // Refusing rather than overwriting: an archive somebody already has is not this
    // operation's to replace.
    if (m_request.targetFileSystem->stat(m_request.target).ok()) {
        fail(VfsError::make(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(m_request.target.fileName())));
        return;
    }

    // Refused rather than written in the clear. Someone who typed a password and got
    // an archive anybody can open has been quietly lied to.
    if (!m_request.passphrase.isEmpty() && !formatSupportsPassword(m_request.format)) {
        fail(VfsError::make(VfsError::NotSupported,
            QStringLiteral("Only zip can carry a password; %1 cannot")
                .arg(suffixFor(m_request.format).mid(1))));
        return;
    }

    QList<Item> items;
    if (!plan(items)) {
        if (isCancelRequested())
            return;
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("There is nothing to compress")));
        return;
    }

    // Refused before a byte is written rather than failing on the second entry with
    // "Raw format only supports one entry per archive", which is true and useless.
    if (takesOneFileOnly(m_request.format)) {
        const int files = static_cast<int>(
            std::count_if(items.cbegin(), items.cend(), [](const Item& item) { return !item.isDirectory; }));
        if (items.size() != 1 || files != 1) {
            fail(VfsError::make(VfsError::NotSupported,
                QStringLiteral("A bare %1 holds one file and no folders; pack %2 as tar.xz instead")
                    .arg(suffixFor(m_request.format).mid(1),
                        items.size() == 1 ? QStringLiteral("a folder") : QStringLiteral("several items"))));
            return;
        }
    }

    Result<std::unique_ptr<QIODevice>> opened = m_request.targetFileSystem->openWrite(m_request.target);
    if (!opened.ok()) {
        fail(opened.error());
        return;
    }
    std::unique_ptr<QIODevice> device = std::move(opened.value());

    WriteContext context;
    context.device = device.get();

    archive* writer = archive_write_new();
    if (!writer) {
        fail(VfsError::make(VfsError::IoError, QStringLiteral("Could not start writing an archive")));
        return;
    }

    switch (m_request.format) {
    case Format::Zip:
        archive_write_set_format_zip(writer);
        break;
    case Format::TarGz:
        archive_write_set_format_pax_restricted(writer);
        archive_write_add_filter_gzip(writer);
        break;
    case Format::TarXz:
        archive_write_set_format_pax_restricted(writer);
        archive_write_add_filter_xz(writer);
        break;
    case Format::SevenZip:
        archive_write_set_format_7zip(writer);
        break;
    case Format::Xz:
        // No container at all: one compressed stream, which is what a bare .xz is.
        archive_write_set_format_raw(writer);
        archive_write_add_filter_xz(writer);
        break;
    }

    if (!m_request.passphrase.isEmpty()) {
        // AES-256 rather than zip's original scheme, which is broken and known to be.
        if (archive_write_set_options(writer, "zip:encryption=aes256") != ARCHIVE_OK
            || archive_write_set_passphrase(writer, m_request.passphrase.toUtf8().constData())
                != ARCHIVE_OK) {
            const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
            archive_write_free(writer);
            device.reset();
            discardPartialArchive();
            fail(VfsError::make(VfsError::IoError,
                reason.isEmpty() ? QStringLiteral("This libarchive cannot encrypt zip archives") : reason));
            return;
        }
    }

    if (archive_write_open(writer, &context, nullptr, writeToDevice, nullptr) != ARCHIVE_OK) {
        const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
        archive_write_free(writer);
        device.reset();
        discardPartialArchive();
        fail(VfsError::make(VfsError::IoError, reason));
        return;
    }

    const auto abandon = [&](const VfsError& error) {
        archive_write_close(writer);
        archive_write_free(writer);
        device.reset();
        discardPartialArchive();
        fail(error);
    };

    QByteArray chunk;
    for (const Item& item : items) {
        if (isCancelRequested()) {
            // A cancelled compression leaves nothing behind: an archive that exists
            // is one that finished.
            archive_write_close(writer);
            archive_write_free(writer);
            device.reset();
            discardPartialArchive();
            return;
        }

        // Opened *before* the header goes in. Writing a header that promises N
        // bytes and then skipping the file leaves that promise unkept and corrupts
        // the stream -- which showed up as a test that failed about one run in
        // three, depending on whether the walker happened to reach the unreadable
        // file before or after a good one.
        std::unique_ptr<QIODevice> in;
        if (!item.isDirectory) {
            Result<std::unique_ptr<QIODevice>> source = m_request.sourceFileSystem->openRead(item.source);
            if (!source.ok()) {
                // One unreadable file is recorded and skipped, the way the walker
                // treats a directory it cannot enter: the rest of the archive is
                // still worth having.
                m_failures.append(QStringLiteral("%1: %2").arg(item.archivePath, source.error().message));
                continue;
            }
            in = std::move(source.value());
        }

        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.archivePath.toUtf8().constData());
        archive_entry_set_size(entry, item.isDirectory ? 0 : item.size);
        archive_entry_set_filetype(entry, item.isDirectory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, item.isDirectory ? 0755 : 0644);

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
            archive_entry_free(entry);
            abandon(VfsError::make(VfsError::IoError, reason));
            return;
        }
        archive_entry_free(entry);

        if (item.isDirectory) {
            ++m_packed;
            continue;
        }

        qint64 remaining = item.size;
        while (remaining > 0 && !in->atEnd()) {
            if (isCancelRequested()) {
                abandon(VfsError::make(VfsError::Cancelled, QStringLiteral("Cancelled")));
                return;
            }
            chunk = in->read(std::min<qint64>(kChunkBytes, remaining));
            if (chunk.isEmpty())
                break;
            if (archive_write_data(writer, chunk.constData(), static_cast<size_t>(chunk.size())) < 0) {
                const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
                abandon(VfsError::make(VfsError::IoError, reason));
                return;
            }
            remaining -= chunk.size();
        }

        ++m_packed;
        setProgress(static_cast<int>(m_packed * 100 / items.size()));
        setStatusText(QStringLiteral("%1 of %2").arg(m_packed).arg(items.size()));
    }

    if (archive_write_close(writer) != ARCHIVE_OK) {
        const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
        archive_write_free(writer);
        device.reset();
        discardPartialArchive();
        fail(VfsError::make(VfsError::IoError, reason));
        return;
    }
    archive_write_free(writer);
    device.reset();

    if (!context.error.isEmpty()) {
        discardPartialArchive();
        fail(VfsError::make(VfsError::IoError, context.error));
        return;
    }

    const QLocale locale;
    setProgress(100);
    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 packed into %2").arg(locale.toString(m_packed), m_request.target.fileName())
            : QStringLiteral("%1 packed, %2 could not be read")
                  .arg(locale.toString(m_packed), locale.toString(m_failures.size())));
}

} // namespace mole
