#include "core/text/ImportJsonLinesTask.h"

#include "core/data/FileType.h"
#include "core/text/DelimitedStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QScopeGuard>
#include <QSet>
#include <QStringDecoder>

#include <optional>

namespace mole {
namespace {

    /// Read size, and rows held before a batch goes to the store. The delimited
    /// importer's own figures: big enough that the per-read overhead vanishes,
    /// small enough that cancelling an import of a huge file is responsive.
    constexpr qint64 kChunkBytes = 1024 * 1024;
    constexpr int kBatchRows = 5000;

    /// The text of one value, as a cell.
    QString cellFor(const QJsonValue& value)
    {
        switch (value.type()) {
        case QJsonValue::String:
            // Itself, not its JSON: a cell full of quoted strings would be a
            // grid nobody can read, and the quotes are the encoding rather than
            // the value.
            return value.toString();
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double:
            // Through QJsonDocument rather than QString::number, so 1e30 and 0.1
            // read back as what the file said rather than as this layer's idea
            // of how to print a double.
            return QString::fromUtf8(QJsonDocument(QJsonArray { value }).toJson(QJsonDocument::Compact))
                .mid(1)
                .chopped(1);
        case QJsonValue::Null:
            // The word, because a cell that was empty for null and empty for
            // absent would lose the difference this deliberately keeps.
            return QStringLiteral("null");
        case QJsonValue::Object:
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                .trimmed();
        case QJsonValue::Array:
            return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)).trimmed();
        case QJsonValue::Undefined:
            break;
        }
        return {};
    }

} // namespace

QStringList jsonKeysInTextOrder(const QString& line, const QJsonObject& record)
{
    QStringList ordered;
    QSet<QString> taken;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    qsizetype stringFrom = -1;
    QString lastString;
    bool lastStringAtTop = false;

    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\')) {
                escaped = true;
            } else if (c == QLatin1Char('"')) {
                inString = false;
                lastString = line.mid(stringFrom, i - stringFrom);
                lastStringAtTop = depth == 1;
            }
            continue;
        }

        switch (c.unicode()) {
        case '"':
            inString = true;
            stringFrom = i + 1;
            break;
        case '{':
        case '[':
            ++depth;
            // A string followed by `{` or `[` was a key with a structure under
            // it, and the string is spent either way.
            lastStringAtTop = false;
            break;
        case '}':
        case ']':
            --depth;
            lastStringAtTop = false;
            break;
        case ':':
            // At depth one, the string before the colon is a top-level key. A
            // colon inside a value is either in a string, which never reaches
            // here, or at a deeper level.
            if (lastStringAtTop && !taken.contains(lastString)) {
                taken.insert(lastString);
                ordered.append(lastString);
            }
            lastStringAtTop = false;
            break;
        default:
            break;
        }
    }

    // Only what the parser confirmed, so a scan that went wrong cannot add a
    // column, and then whatever it missed -- in QJsonObject's own order, which is
    // sorted, because there is nothing better left to go on.
    QStringList keys;
    keys.reserve(record.size());
    for (const QString& key : ordered) {
        if (record.contains(key))
            keys.append(key);
    }
    for (auto it = record.constBegin(); it != record.constEnd(); ++it) {
        if (!keys.contains(it.key()))
            keys.append(it.key());
    }
    return keys;
}

QStringList jsonRecordAsRow(const QJsonObject& record, const QStringList& headers, qint64* unseen)
{
    QStringList row;
    row.reserve(headers.size());
    for (const QString& key : headers) {
        const auto it = record.constFind(key);
        // Absent is an empty cell, and it is not the same as null above.
        row.append(it == record.constEnd() ? QString() : cellFor(it.value()));
    }

    if (unseen) {
        for (auto it = record.constBegin(); it != record.constEnd(); ++it) {
            if (!headers.contains(it.key()))
                ++*unseen;
        }
    }
    return row;
}

ImportJsonLinesTask::ImportJsonLinesTask(
    FileSystemPtr fileSystem, VfsUri target, std::shared_ptr<DelimitedStore> store, QObject* parent)
    : Task(QStringLiteral("Import %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
    , m_store(std::move(store))
{
    noteRunsOn(m_fileSystem);
}

QStringList ImportJsonLinesTask::keysIn(const QString& sample, bool* sawAnObject)
{
    QStringList keys;
    QSet<QString> seen;
    if (sawAnObject)
        *sawAnObject = false;

    // Every line, the last one included, and the parser sorts out whether it is
    // whole. The sample ends wherever the byte count ran out, so its final line
    // may be half a record -- and no prefix of a JSON object parses as one,
    // because its braces cannot balance until the end. So a cut record
    // contributes nothing and needs no special case, while a file smaller than
    // the sample with no trailing newline still has its last record counted.
    const QStringList lines = sample.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        const QJsonDocument parsed = QJsonDocument::fromJson(line.toUtf8());
        if (!parsed.isObject())
            continue;
        if (sawAnObject)
            *sawAnObject = true;

        const QJsonObject record = parsed.object();
        for (const QString& key : jsonKeysInTextOrder(line, record)) {
            if (!seen.contains(key)) {
                seen.insert(key);
                keys.append(key);
            }
        }
    }
    return keys;
}

void ImportJsonLinesTask::run()
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

    // The encoding comes from the head of the file rather than being assumed.
    // This was UTF-8 outright, and never checked hasError() either -- so a
    // cp1252 export imported with U+FFFD in place of every accented character
    // and said nothing about it. FileType::encodingFor() is the same answer the
    // sniffer gives, so what the preview calls text and what this reads are one
    // decision. See MOLE-405.
    std::optional<QStringDecoder> decoder;
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

    // The columns, from the head of the file and then held. Fails the import only
    // when the store refuses; a file with no records in its sample is not a
    // failure, it is a file to show the source of.
    const auto settleShape = [&](const QString& sample) -> bool {
        m_headers = keysIn(sample, &m_looksLikeRecords);
        if (!m_looksLikeRecords || m_headers.isEmpty())
            return false;

        QString error;
        if (!m_store->beginImport(m_headers, &error)) {
            fail(VfsError::make(VfsError::IoError, error));
            return false;
        }
        started = true;
        emit shapeSettled();
        return true;
    };

    // One line at a time out of whatever has arrived, leaving anything after the
    // last newline for the next chunk.
    const auto takeCompleteLines = [&](bool atEnd) {
        qsizetype from = 0;
        while (true) {
            const qsizetype breakAt = pending.indexOf(QLatin1Char('\n'), from);
            const bool last = breakAt < 0;
            if (last && !atEnd)
                break;
            const QString line = (last ? pending.mid(from) : pending.mid(from, breakAt - from)).trimmed();
            from = last ? pending.size() : breakAt + 1;

            if (!line.isEmpty()) {
                const QJsonDocument parsed = QJsonDocument::fromJson(line.toUtf8());
                if (parsed.isObject()) {
                    batch.append(jsonRecordAsRow(parsed.object(), m_headers, &m_keysWithoutAColumn));
                } else {
                    // A blank line is skipped and not counted; anything else that
                    // is not an object is counted, because it is a record the
                    // reader cannot see and has a right to know about.
                    ++m_skippedLines;
                }
            }
            if (last)
                break;
        }
        pending = pending.mid(from);
    };

    while (!device->atEnd()) {
        if (isCancelRequested())
            return;

        const QByteArray chunk = device->read(kChunkBytes);
        if (chunk.isEmpty())
            break;
        bytesRead += chunk.size();
        if (!decoder) {
            decoder.emplace(FileType::encodingFor(
                QByteArrayView(chunk).first(qMin<qsizetype>(chunk.size(), FileType::kSampleBytes))));
        }
        pending += decoder->decode(chunk);
        // Sticky, and reported at the end: a file with one bad byte in it is
        // still worth importing, and a reader has to be told the cells they are
        // looking at are not what the file said.
        if (decoder->hasError())
            m_undecodedBytes = true;

        if (!started) {
            if (pending.size() < kSampleBytes && !device->atEnd())
                continue;
            if (!settleShape(pending.left(kSampleBytes)))
                return;
        }

        takeCompleteLines(false);
        if (batch.size() >= kBatchRows && !flush())
            return;

        if (totalBytes > 0) {
            setProgress(static_cast<int>(bytesRead * 100 / totalBytes));
            setStatusText(QStringLiteral("%1 records").arg(m_importedRows + batch.size()));
        }
    }

    if (isCancelRequested())
        return;

    // A file smaller than the sample never entered the branch above, and a file
    // with no trailing newline has one last record.
    if (!started && !settleShape(pending.left(kSampleBytes)))
        return;
    takeCompleteLines(true);

    if (!flush())
        return;

    QString error;
    if (!m_store->endImport(&error)) {
        fail(VfsError::make(VfsError::IoError, error));
        return;
    }
    setProgress(100);
    setStatusText(m_undecodedBytes
            ? QStringLiteral("%1 records · some characters could not be read").arg(m_importedRows)
            : QStringLiteral("%1 records").arg(m_importedRows));
}

} // namespace mole
