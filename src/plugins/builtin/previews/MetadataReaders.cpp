#include "plugins/builtin/previews/MetadataReaders.h"

#include "core/data/FileType.h"
#include "core/diagnostics/Diagnostics.h"

#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>

namespace mole {

QList<FileFact> GenericMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    Q_UNUSED(head);
    Q_UNUSED(services);
    Q_UNUSED(cancel);

    static const QMimeDatabase mimeDatabase;
    // What the file is when something has looked inside it, and what its name
    // suggests when nothing has. See ADR-0033.
    const QMimeType type = entry.mimeType.isEmpty()
        ? mimeDatabase.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension)
        : mimeDatabase.mimeTypeForName(entry.mimeType);

    const QLocale locale;
    QList<FileFact> facts;
    facts.append({ QStringLiteral("Type"),
        type.isValid() && !type.isDefault() ? type.comment() : QStringLiteral("Unknown") });
    facts.append({ QStringLiteral("MIME type"), type.name() });
    facts.append({ QStringLiteral("Size"),
        QStringLiteral("%1 (%2 bytes)")
            .arg(locale.formattedDataSize(entry.size))
            .arg(locale.toString(entry.size)) });
    if (entry.modified.isValid())
        facts.append({ QStringLiteral("Modified"), locale.toString(entry.modified, QLocale::LongFormat) });
    facts.append({ QStringLiteral("Location"), entry.uri.parent().toString() });
    facts.append({ QStringLiteral("Full path"), entry.uri.toString() });
    facts.append(
        { QStringLiteral("Readable"), entry.isReadable ? QStringLiteral("yes") : QStringLiteral("no") });
    facts.append(
        { QStringLiteral("Writable"), entry.isWritable ? QStringLiteral("yes") : QStringLiteral("no") });
    return facts;
}

ReadMetadataTask::ReadMetadataTask(FileSystemPtr fileSystem, FileEntry entry, QByteArray head,
    QList<IMetadataReader*> readers, PluginServices services, QObject* parent)
    : Task(QStringLiteral("Details of %1").arg(entry.name), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_entry(std::move(entry))
    , m_head(std::move(head))
    , m_readers(std::move(readers))
    , m_services(services)
{
    // One of a crowd: the details drawer reads whatever the cursor lands on, so walking
    // a folder with the arrow keys is one of these per row. See Task::isOneOfMany() and ADR-0064.
    setOneOfMany(true);
}

void ReadMetadataTask::run()
{
    // The head, if the preview did not already have one to hand over. Bounded
    // by the same page the type sniff uses: nothing here reads a file.
    if (m_head.isEmpty() && m_entry.size != 0 && m_fileSystem) {
        Result<std::unique_ptr<QIODevice>> opened = m_fileSystem->openRead(m_entry.uri);
        if (opened.ok() && opened.value()) {
            m_head = opened.value()->read(FileType::kSampleBytes);
            m_bytesRead = m_head.size();
        }
    }

    for (IMetadataReader* reader : std::as_const(m_readers)) {
        if (isCancelRequested())
            return;
        if (!reader)
            continue;

        // One reader failing is one reader's rows missing. The panel is a set
        // of independent claims about a file, and a plugin that throws must not
        // take the others' answers with it.
        try {
            const QList<FileFact> said = reader->read(m_entry, m_head, m_services, cancelToken());
            if (!said.isEmpty()) {
                m_blockStarts.append(int(m_facts.size()));
                m_facts.append(said);
            }
        } catch (const std::exception& error) {
            // On the log's subject for a job, like every other line about one:
            // this runs inside a Task, and a bare qWarning is a line MOLE_LOG=task
            // does not show and a log filtered by subject loses. See ADR-0012.
            qCWarning(taskLog, "metadata reader %s failed: %s", qPrintable(reader->id()), error.what());
        } catch (...) {
            qCWarning(taskLog, "metadata reader %s failed", qPrintable(reader->id()));
        }
    }

    setProgress(100);
}

} // namespace mole
