#include "core/text/ImportDelimitedTask.h"

#include "core/text/DelimitedStore.h"
#include "core/text/DelimitedStreamParser.h"
#include "core/text/DelimitedText.h"

#include <QScopeGuard>
#include <QStringDecoder>

#include <algorithm>

namespace mole {
namespace {

    /// Read size. Big enough that the per-read overhead vanishes, small enough
    /// that cancelling an import of a huge file is responsive.
    constexpr qint64 kChunkBytes = 1024 * 1024;
    /// Rows held before handing a batch to the store.
    constexpr int kBatchRows = 5000;
    /// How much of the file to look at when guessing the separator.
    constexpr int kDetectionSample = 64 * 1024;
    /// Rows inspected when deciding how many columns the table has.
    constexpr int kShapeSample = 1000;

    /// Names for the columns, widened to the widest row in the sample.
    ///
    /// A header shorter than the rows under it is common -- an export with a
    /// trailing unnamed column, or a separator the header happens not to contain.
    /// Sizing the table to the header alone would silently drop those fields.
    QStringList buildHeaders(const QList<QStringList>& rows, QStringList headers)
    {
        int columns = static_cast<int>(headers.size());
        const int inspect = static_cast<int>(std::min<qsizetype>(rows.size(), kShapeSample));
        for (int i = 0; i < inspect; ++i)
            columns = std::max(columns, static_cast<int>(rows.at(i).size()));

        // Unnamed columns are left empty; the model shows a spreadsheet letter
        // rather than inventing a name that looks like it came from the file.
        while (headers.size() < columns)
            headers.append(QString());
        return headers;
    }

} // namespace

ImportDelimitedTask::ImportDelimitedTask(
    FileSystemPtr fileSystem, VfsUri target, std::shared_ptr<DelimitedStore> store, QObject* parent)
    : Task(QStringLiteral("Import %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
    , m_store(std::move(store))
{
}

void ImportDelimitedTask::run()
{
    // The store is let go of the moment this returns, whichever way it returns.
    // A finished task stays in the task list for an hour so somebody can see what
    // ran, and holding the store that long would keep a scratch database, its
    // connections and its temporary directory alive for every file previewed.
    const auto release = qScopeGuard([this] { m_store.reset(); });

    if (!m_fileSystem || !m_store) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted for this file")));
        return;
    }

    qint64 totalBytes = -1;
    if (Result<FileEntry> stat = m_fileSystem->stat(m_target); stat.ok())
        totalBytes = stat.value().size;

    Result<std::unique_ptr<QIODevice>> opened = m_fileSystem->openRead(m_target);
    if (!opened.ok()) {
        fail(opened.error());
        return;
    }
    std::unique_ptr<QIODevice> device = std::move(opened.value());
    if (!device) {
        fail(VfsError::make(VfsError::IoError, QStringLiteral("The file could not be opened")));
        return;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    DelimitedStreamParser parser;
    QString pending;
    qint64 bytesRead = 0;
    bool started = false;

    QList<QStringList> batch;
    batch.reserve(kBatchRows);

    const auto flush = [&]() -> bool {
        if (batch.isEmpty())
            return true;
        QString error;
        if (!m_store->addRows(batch, &error)) {
            fail(VfsError::make(VfsError::IoError, error));
            return false;
        }
        m_importedRows += batch.size();
        batch.clear();
        emit rowsImported(m_importedRows);
        return true;
    };

    while (!device->atEnd()) {
        if (isCancelRequested())
            return;

        const QByteArray chunk = device->read(kChunkBytes);
        if (chunk.isEmpty())
            break;
        bytesRead += chunk.size();

        pending += decoder.decode(chunk);

        // The separator is guessed once, from the head of the file, and then
        // held. Re-guessing per chunk would silently change the shape of the
        // table halfway down.
        if (!started) {
            const bool haveEnough = pending.size() >= kDetectionSample || device->atEnd();
            if (!haveEnough)
                continue;

            if (m_separator.isNull())
                m_separator = DelimitedTextParser::detectSeparator(pending.left(kDetectionSample));
            parser.setSeparator(m_separator);

            QList<QStringList> rows = parser.feed(pending);
            pending.clear();

            QStringList headers;
            if (m_firstRowIsHeader && !rows.isEmpty())
                headers = rows.takeFirst();
            headers = buildHeaders(rows, headers);
            if (headers.isEmpty())
                continue;

            QString error;
            if (!m_store->beginImport(headers, &error)) {
                fail(VfsError::make(VfsError::IoError, error));
                return;
            }
            started = true;
            // Announced now rather than at the end: the shape is settled here,
            // and the rows below are about to become visible under a caption
            // that would otherwise still be showing the default guess.
            emit separatorDetected(m_separator);
            batch.append(rows);
        } else {
            batch.append(parser.feed(pending));
            pending.clear();
        }

        if (batch.size() >= kBatchRows && !flush())
            return;

        if (totalBytes > 0) {
            setProgress(static_cast<int>(bytesRead * 100 / totalBytes));
            setStatusText(QStringLiteral("%1 rows").arg(m_importedRows + batch.size()));
        }
    }

    if (isCancelRequested())
        return;

    // Whatever is left: a file smaller than the detection sample never entered
    // the branch above, and a file with no trailing newline has one last row.
    if (!started) {
        if (m_separator.isNull())
            m_separator = DelimitedTextParser::detectSeparator(pending.left(kDetectionSample));
        parser.setSeparator(m_separator);

        QList<QStringList> rows = parser.feed(pending);
        pending.clear();
        const QStringList tail = parser.finish();
        if (!tail.isEmpty())
            rows.append(tail);

        QStringList headers;
        if (m_firstRowIsHeader && !rows.isEmpty())
            headers = rows.takeFirst();
        headers = buildHeaders(rows, headers);

        if (headers.isEmpty()) {
            fail(VfsError::make(VfsError::NotSupported, QStringLiteral("This file has no rows")));
            return;
        }

        QString error;
        if (!m_store->beginImport(headers, &error)) {
            fail(VfsError::make(VfsError::IoError, error));
            return;
        }
        batch.append(rows);
    } else {
        batch.append(parser.feed(pending));
        const QStringList tail = parser.finish();
        if (!tail.isEmpty())
            batch.append(tail);
    }

    if (!flush())
        return;

    QString error;
    if (!m_store->endImport(&error)) {
        fail(VfsError::make(VfsError::IoError, error));
        return;
    }

    setProgress(100);
    setStatusText(QStringLiteral("%1 rows").arg(m_importedRows));
}

} // namespace mole
