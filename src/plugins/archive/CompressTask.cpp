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
    noteTouching(m_request.sources);
    noteTouching(m_request.target);
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

        // The header above has already promised exactly item.size bytes, and
        // libarchive keeps that promise whatever arrives: an entry that comes up
        // short is padded to the declared size with zeros, and one that would
        // run over is cut off at it. Either way the archive is structurally
        // perfect and the member is not the file -- and this is the operation
        // that then offers to delete the original. So every byte is accounted
        // for, and anything else abandons the whole archive rather than closing
        // one that cannot be trusted. See MOLE-338 and ADR-0027.
        //
        // Read into a buffer rather than through the QByteArray overload,
        // because that one answers "the file ended" and "the read failed" with
        // the same empty result -- which is the difference between a source that
        // shrank and a connection that died.
        qint64 remaining = item.size;
        chunk.resize(kChunkBytes);
        while (remaining > 0) {
            if (isCancelRequested()) {
                abandon(VfsError::make(VfsError::Cancelled, QStringLiteral("Cancelled")));
                return;
            }
            const qint64 got = in->read(chunk.data(), std::min<qint64>(kChunkBytes, remaining));
            if (got < 0) {
                const QString why = QStringLiteral("%1: the source stopped after %2 bytes: %3")
                                        .arg(item.archivePath)
                                        .arg(item.size - remaining)
                                        .arg(in->errorString());
                m_failures.append(why);
                abandon(VfsError::make(VfsError::IoError, why));
                return;
            }
            if (got == 0)
                break;
            if (archive_write_data(writer, chunk.constData(), static_cast<size_t>(got)) < 0) {
                const QString reason = QString::fromLocal8Bit(archive_error_string(writer));
                abandon(VfsError::make(VfsError::IoError, reason));
                return;
            }
            remaining -= got;
        }

        if (remaining != 0) {
            // Not asked of the source the way ADR-0027 asks it. A transfer can
            // accept a file that really did shrink, because it writes what
            // arrived; here the size is already in the stream and cannot be
            // taken back, so both readings of a short read have the same answer.
            const QString why = QStringLiteral("%1: the listing said %2 bytes and %3 arrived")
                                    .arg(item.archivePath)
                                    .arg(item.size)
                                    .arg(item.size - remaining);
            m_failures.append(why);
            abandon(VfsError::make(VfsError::IoError, why));
            return;
        }

        // And the other direction, which is silent truncation: a log written to
        // between the stat and the read has more to give than the header allows.
        char beyond = 0;
        if (in->read(&beyond, 1) > 0) {
            const QString why = QStringLiteral("%1: it is longer than the %2 bytes the listing said, "
                                               "so it would have been packed short")
                                    .arg(item.archivePath)
                                    .arg(item.size);
            m_failures.append(why);
            abandon(VfsError::make(VfsError::IoError, why));
            return;
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

    // A buffered backend only sends the archive when the stream is closed, so
    // until this succeeds nothing has been written to a remote target at all.
    // The failure paths above reset the device instead, which abandons the
    // payload on purpose -- they go on to discard the partial archive anyway.
    const Result<void> committed = closeAndReport(*device);
    device.reset();

    if (!committed.ok()) {
        discardPartialArchive();
        fail(committed.error());
        return;
    }

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

    if (m_request.removeSourcesWhenDone)
        removeSources();
}

void CompressTask::removeSources()
{
    const QLocale locale;

    // The archive has to be readable as a whole before the originals go. Anything
    // that could not be read is missing from it, so deleting now would be deleting
    // the only copy -- the archive is kept and the sources are left alone.
    if (!m_failures.isEmpty()) {
        setStatusText(QStringLiteral("%1 packed; the originals were kept because %2 could not be read")
                          .arg(locale.toString(m_packed), locale.toString(m_failures.size())));
        return;
    }

    // Asked for and then cancelled between the last entry and here: the archive
    // stands, but nothing gets deleted on the strength of a job that was stopped.
    if (isCancelRequested()) {
        setStatusText(QStringLiteral("%1 packed; the originals were kept").arg(locale.toString(m_packed)));
        return;
    }

    QStringList refused;
    for (const VfsUri& source : m_request.sources) {
        // Packing the folder you are standing in puts the archive inside it, so
        // deleting that folder would delete the archive with it. This is the case
        // that would turn "keep the archive, drop the files" into keeping nothing.
        const QString sourcePath = source.toString();
        const QString targetPath = m_request.target.toString();
        if (targetPath == sourcePath || targetPath.startsWith(sourcePath + QLatin1Char('/'))) {
            refused.append(source.fileName());
            continue;
        }

        Result<FileEntry> found = m_request.sourceFileSystem->stat(source);
        const bool isDirectory = found.ok() && found.value().isDir;
        Result<void> gone = m_request.sourceFileSystem->remove(source, isDirectory);
        if (gone.ok())
            m_removed.append(source);
        else
            refused.append(source.fileName());
    }

    if (refused.isEmpty()) {
        setStatusText(QStringLiteral("%1 packed into %2, %3 removed")
                          .arg(locale.toString(m_packed), m_request.target.fileName(),
                              locale.toString(m_removed.size())));
        return;
    }

    // Reported rather than failed: the archive is written and correct, which is the
    // part that cannot be repeated. What is left is a deletion somebody can retry.
    setStatusText(QStringLiteral("%1 packed into %2; %3 could not be removed: %4")
                      .arg(locale.toString(m_packed), m_request.target.fileName(),
                          locale.toString(refused.size()), refused.join(QStringLiteral(", "))));
}

} // namespace mole
