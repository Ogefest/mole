#include "core/data/FileType.h"

#include <QMimeDatabase>
#include <QMimeType>
#include <QStringDecoder>

namespace mole {

namespace {

    const QMimeDatabase& database()
    {
        // Built once: opening the shared database is cheap but not free, and this
        // is asked twice for every file that reaches the fallback tier.
        static const QMimeDatabase instance;
        return instance;
    }

    QString plainText()
    {
        return QStringLiteral("text/plain");
    }
    QString binary()
    {
        return QStringLiteral("application/octet-stream");
    }

    bool isText(const QMimeType& type)
    {
        return type.inherits(plainText());
    }

    /// A QByteArray over the caller's bytes, which QMimeDatabase wants and which
    /// it does not keep. No copy: the sample is a page long and this is on the
    /// path of every preview.
    QByteArray borrow(QByteArrayView sample)
    {
        return QByteArray::fromRawData(sample.data(), sample.size());
    }

    bool startsWithByteOrderMark(QByteArrayView sample)
    {
        static const QByteArray marks[] = { QByteArrayLiteral("\xef\xbb\xbf"), QByteArrayLiteral("\xff\xfe"),
            QByteArrayLiteral("\xfe\xff") };
        for (const QByteArray& mark : marks) {
            if (sample.startsWith(mark))
                return true;
        }
        return false;
    }

    bool tooMany(qsizetype controls, qsizetype size)
    {
        return controls * 100 > size * FileType::kControlPercent;
    }

} // namespace

bool FileType::looksLikeText(QByteArrayView sample)
{
    if (sample.isEmpty())
        return true;
    if (startsWithByteOrderMark(sample))
        return true;

    qsizetype c0Controls = 0;
    qsizetype c1Controls = 0;
    for (const char raw : sample) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == 0)
            return false;
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f')
            ++c0Controls;
        else if (byte >= 0x80 && byte <= 0x9f)
            ++c1Controls;
    }
    if (tooMany(c0Controls, sample.size()))
        return false;

    QStringDecoder utf8(QStringConverter::Utf8);
    const QString decoded = utf8.decode(sample);
    Q_UNUSED(decoded);
    if (!utf8.hasError())
        return true;

    // Not UTF-8, so read as Latin-1, where the C1 range is control characters as
    // well. A Latin-1 log has none of them; binary data is full of them.
    return !tooMany(c1Controls, sample.size());
}

QString FileType::identify(const QString& name, QByteArrayView head)
{
    const QMimeDatabase& mimeDatabase = database();

    // What the name says. Globs only -- Qt is never given the path, so nothing
    // here can touch the file however the name is spelled.
    const QMimeType byName = mimeDatabase.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    const QMimeType named = byName.isDefault() ? QMimeType() : byName;
    const bool nameSaysText = named.isValid() && isText(named);

    // An empty file. The database has a type for one (application/x-zerosize),
    // and it is no use to a viewer; nothing in an empty file is binary.
    if (head.isEmpty())
        return nameSaysText ? named.name() : plainText();

    // What the bytes say: the magic rules, and then Qt's own text test, so the
    // answer is a format, a bare text/plain, or the default when the sample made
    // no sense to either.
    const QMimeType byData = mimeDatabase.mimeTypeForData(borrow(head));
    const bool bytesAreText = byData.isDefault() ? looksLikeText(head) : isText(byData);
    // A claim about the format, as opposed to "these bytes read as text", which
    // says nothing a name could contradict.
    const QMimeType magic = byData.isDefault() || byData.name() == plainText() ? QMimeType() : byData;

    if (magic.isValid()) {
        if (named.isValid() && (named.inherits(magic.name()) || (nameSaysText && bytesAreText)))
            return named.name();
        return magic.name();
    }

    if (named.isValid() && nameSaysText == bytesAreText)
        return named.name();
    return bytesAreText ? plainText() : binary();
}

} // namespace mole
