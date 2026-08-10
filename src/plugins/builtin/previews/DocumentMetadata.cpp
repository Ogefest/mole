#include "plugins/builtin/previews/DocumentMetadata.h"

#include "core/vfs/VfsManager.h"

#include <QDateTime>
#include <QLocale>
#include <QXmlStreamReader>

#ifdef MOLE_HAVE_ARCHIVE
#include "plugins/archive/ZipPrefix.h"
#endif

namespace mole {
namespace {

    const QString kCoreProperties = QStringLiteral("docProps/core.xml");
    const QString kAppProperties = QStringLiteral("docProps/app.xml");
    const QString kOpenDocumentMeta = QStringLiteral("meta.xml");

    /// Every element name whose text is worth a row, and what to call it.
    ///
    /// Matched on the local name rather than on the namespace prefix: the
    /// prefixes are conventional (`dc:`, `cp:`, `meta:`) but not required, and a
    /// writer using its own is writing valid XML.
    struct Wanted
    {
        const char* element;
        const char* label;
    };

    QString labelFor(const QString& localName, const QList<Wanted>& wanted)
    {
        for (const Wanted& entry : wanted) {
            if (localName == QLatin1String(entry.element))
                return QString::fromLatin1(entry.label);
        }
        return {};
    }

    /// A date the way a document writes it, as a reader would say it.
    QString dateText(const QString& raw)
    {
        QDateTime when = QDateTime::fromString(raw, Qt::ISODate);
        if (!when.isValid())
            when = QDateTime::fromString(raw, Qt::ISODateWithMs);
        return when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : raw;
    }

    bool isDateLabel(const QString& label)
    {
        return label == QLatin1String("Created") || label == QLatin1String("Modified")
            || label == QLatin1String("Printed");
    }

    /// Walks one properties document and appends what it names.
    ///
    /// The reader is given the bytes and nothing else: `QXmlStreamReader` does
    /// not fetch a DTD or resolve an external entity of its own accord, and
    /// nothing here asks it to. An entity it cannot resolve is an error on that
    /// element, which costs that row.
    void readProperties(
        QByteArrayView xml, const QList<Wanted>& wanted, QList<FileFact>& facts, QStringList& seen)
    {
        if (xml.isEmpty())
            return;

        QXmlStreamReader reader(QByteArray::fromRawData(xml.data(), xml.size()));
        while (!reader.atEnd()) {
            if (reader.readNext() != QXmlStreamReader::StartElement)
                continue;

            const QString label = labelFor(reader.name().toString(), wanted);
            if (label.isEmpty() || seen.contains(label))
                continue;

            const QString value = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            if (value.isEmpty())
                continue;

            facts.append({ label, isDateLabel(label) ? dateText(value) : value });
            seen.append(label);
        }
    }

    /// OpenDocument keeps its counts in the attributes of one element rather than
    /// in elements of their own.
    void readOpenDocumentStatistics(QByteArrayView xml, QList<FileFact>& facts, QStringList& seen)
    {
        if (xml.isEmpty())
            return;

        QXmlStreamReader reader(QByteArray::fromRawData(xml.data(), xml.size()));
        while (!reader.atEnd()) {
            if (reader.readNext() != QXmlStreamReader::StartElement)
                continue;
            if (reader.name() != QLatin1String("document-statistic"))
                continue;

            const QXmlStreamAttributes attributes = reader.attributes();
            const auto append = [&](const char* attribute, const QString& label) {
                for (const QXmlStreamAttribute& candidate : attributes) {
                    if (candidate.name() != QLatin1String(attribute) || seen.contains(label))
                        continue;
                    facts.append({ label, candidate.value().toString() });
                    seen.append(label);
                }
            };
            append("page-count", QStringLiteral("Pages"));
            append("word-count", QStringLiteral("Words"));
            append("character-count", QStringLiteral("Characters"));
            return;
        }
    }

} // namespace

bool DocumentMetadataReader::isAvailable()
{
#ifdef MOLE_HAVE_ARCHIVE
    return true;
#else
    return false;
#endif
}

bool DocumentMetadataReader::canRead(const FileEntry& entry) const
{
    if (entry.isDir || !isAvailable())
        return false;

    static const QStringList suffixes = { QStringLiteral("docx"), QStringLiteral("xlsx"),
        QStringLiteral("pptx"), QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp") };
    if (suffixes.contains(entry.uri.suffix().toLower()))
        return true;

    // OOXML and ODF are zips, and magic alone cannot tell them from any other
    // zip -- so a document whose extension was removed sniffs as
    // application/zip. This claims that too and looks for the members; a zip
    // that is not a document contributes nothing, which is the honest outcome.
    return entry.mimeType == QLatin1String("application/zip")
        || entry.mimeType.contains(QLatin1String("opendocument"))
        || entry.mimeType.contains(QLatin1String("openxmlformats"));
}

QList<FileFact> DocumentMetadataReader::factsFor(QByteArrayView prefix)
{
    QList<FileFact> facts;
#ifdef MOLE_HAVE_ARCHIVE
    static const QList<Wanted> coreWanted {
        { "creator", "Author" },
        { "lastModifiedBy", "Last saved by" },
        { "title", "Title" },
        { "subject", "Subject" },
        { "description", "Description" },
        { "keywords", "Keywords" },
        { "revision", "Revision" },
        { "created", "Created" },
        { "modified", "Modified" },
        // OpenDocument's own spellings, in the same table because one reader
        // serves both and the names do not collide.
        { "initial-creator", "Author" },
        { "creation-date", "Created" },
        { "date", "Modified" },
        { "generator", "Application" },
    };
    static const QList<Wanted> appWanted {
        { "Application", "Application" },
        { "Company", "Company" },
        { "Pages", "Pages" },
        { "Words", "Words" },
        { "Characters", "Characters" },
        { "TotalTime", "Editing time" },
    };

    const QHash<QString, QByteArray> members
        = membersFromZipPrefix(prefix, { kCoreProperties, kAppProperties, kOpenDocumentMeta });
    if (members.isEmpty())
        return facts;

    QStringList seen;
    // Author and title first, because they are what somebody opened the panel
    // for; the counts and the application after them.
    readProperties(members.value(kCoreProperties), coreWanted, facts, seen);
    readProperties(members.value(kOpenDocumentMeta), coreWanted, facts, seen);
    readOpenDocumentStatistics(members.value(kOpenDocumentMeta), facts, seen);
    readProperties(members.value(kAppProperties), appWanted, facts, seen);

    if (facts.isEmpty() && !members.isEmpty())
        facts.append({ QStringLiteral("Properties"), QStringLiteral("none recorded") });
#else
    Q_UNUSED(prefix);
#endif
    return facts;
}

QList<FileFact> DocumentMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    if (!isAvailable())
        return {};

    QByteArray prefix = head.toByteArray();
    if (prefix.size() < kPrefixBytes && services.vfs) {
        if (FileSystemPtr fs = services.vfs->resolve(entry.uri)) {
            Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
            if (opened.ok() && opened.value())
                prefix = opened.value()->read(kPrefixBytes);
        }
    }
    if (cancel.isCancelled())
        return {};

    QList<FileFact> facts = factsFor(prefix);
    // A container whose properties are further in than the prefix reaches says
    // so, rather than saying nothing and reading like a document with no author.
    if (facts.isEmpty() && prefix.size() >= kPrefixBytes) {
        facts.append({ QStringLiteral("Properties"),
            QStringLiteral("not in the first %1").arg(QLocale().formattedDataSize(kPrefixBytes)) });
    }
    return facts;
}

} // namespace mole
