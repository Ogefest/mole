#include "core/search/SearchQuery.h"

#include "core/data/FileType.h"

#include <QJsonArray>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace mole {
namespace {

    /// What a leftover criterion costs once the source has declined it.
    ///
    /// A criterion a source cannot push down does not become expensive; it
    /// becomes something we do ourselves from the entry we already have. What
    /// was going to cost a read still costs a read.
    PredicateCost evaluationCost(const SearchPredicate& predicate)
    {
        return predicate.cost == PredicateCost::PushedDown ? PredicateCost::Cheap : predicate.cost;
    }

    bool indexCanExpress(const SearchPredicate& predicate)
    {
        if (predicate.cost != PredicateCost::PushedDown)
            return false;

        switch (predicate.field) {
        case SearchPredicate::Field::Name:
            // A substring of a column, and only that. A glob or an expression
            // is not a LIKE and must not be turned into one, and a negation and
            // a word boundary are both things SQL would answer differently.
            return predicate.match == SearchPredicate::Match::Contains && !predicate.negate
                && !predicate.wholeWord;
        case SearchPredicate::Field::Extension:
        case SearchPredicate::Field::Kind:
            return !predicate.negate;
        case SearchPredicate::Field::Size:
            return !predicate.negate;
        case SearchPredicate::Field::Modified:
        case SearchPredicate::Field::Created:
        case SearchPredicate::Field::Accessed:
            // `mtime` is a column and no clause reads it yet; the other two are
            // not recorded at all. All three are evaluated rather than ignored,
            // which is the whole point of the plan saying so.
            return false;
        case SearchPredicate::Field::Metadata:
            // Its own table, with an index for each way of asking. This is the
            // one criterion the index answers that a walk has to open a file
            // for, which is the whole reason it is indexed at all.
            return true;
        case SearchPredicate::Field::Hidden:
        case SearchPredicate::Field::TypeClass:
        case SearchPredicate::Field::Content:
        case SearchPredicate::Field::Path:
        case SearchPredicate::Field::Under:
            // Rows hold the path inside their volume rather than the whole uri,
            // nothing records what a file is, and the contents are deliberately
            // not indexed at all -- a full-text index over a disk of files at
            // scale is a second disk.
            return false;
        }
        return false;
    }

    bool containsText(const QString& haystack, const QString& needle, bool caseSensitive, bool wholeWord)
    {
        if (needle.isEmpty())
            return true;
        if (!wholeWord) {
            return caseSensitive ? haystack.contains(needle, Qt::CaseSensitive)
                                 : foldForSearch(haystack).contains(foldForSearch(needle));
        }
        // A word boundary rather than a substring: "report" stops matching
        // "reporting". \b is wrong for a file name, where the separators people
        // mean are punctuation rather than only spaces, so the boundary is
        // stated as "not a letter and not a digit".
        static const QString boundary = QStringLiteral("(?<![\\p{L}\\p{N}])%1(?![\\p{L}\\p{N}])");
        const QRegularExpression word(boundary.arg(QRegularExpression::escape(needle)),
            caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        return word.match(haystack).hasMatch();
    }

    bool matchesPattern(const QString& text, const SearchPredicate& predicate)
    {
        const auto options = predicate.caseSensitive ? QRegularExpression::NoPatternOption
                                                     : QRegularExpression::CaseInsensitiveOption;
        switch (predicate.match) {
        case SearchPredicate::Match::Glob: {
            const QRegularExpression glob(
                QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(
                    predicate.text, QRegularExpression::UnanchoredWildcardConversion)),
                options);
            return glob.match(text).hasMatch();
        }
        case SearchPredicate::Match::Regex: {
            const QRegularExpression pattern(predicate.text, options);
            // A pattern that does not compile matches nothing. Matching
            // everything would turn a typo into a search of the whole disk.
            return pattern.isValid() && pattern.match(text).hasMatch();
        }
        default:
            return containsText(text, predicate.text, predicate.caseSensitive, predicate.wholeWord);
        }
    }

    bool inRange(qint64 value, const SearchPredicate& predicate)
    {
        return predicate.match == SearchPredicate::Match::AtMost ? value <= predicate.number
                                                                 : value >= predicate.number;
    }

    bool timeMatches(const QDateTime& when, const SearchPredicate& predicate)
    {
        // A drive that does not report this date has no answer, which is not
        // the same as a file from the beginning of time.
        if (!when.isValid())
            return false;
        return inRange(when.toSecsSinceEpoch(), predicate);
    }

    SearchPredicate whenPredicate(SearchPredicate::Field field, SearchPredicate::Match match, qint64 epoch)
    {
        SearchPredicate predicate;
        predicate.field = field;
        predicate.match = match;
        predicate.number = epoch;
        return predicate;
    }

} // namespace

bool isUnder(const QString& uri, const QString& prefix)
{
    if (prefix.isEmpty() || uri == prefix)
        return true;
    if (prefix.endsWith(QLatin1Char('/')))
        return uri.startsWith(prefix);
    return uri.startsWith(prefix) && uri.at(prefix.size()) == QLatin1Char('/');
}

QString foldForSearch(const QString& text)
{
    return text.toLower();
}

QStringList knownTypeClasses()
{
    return { QStringLiteral("image"), QStringLiteral("video"), QStringLiteral("audio"),
        QStringLiteral("document"), QStringLiteral("archive"), QStringLiteral("code"),
        QStringLiteral("folder") };
}

QString classOfMimeType(const QString& mimeType)
{
    if (mimeType.startsWith(QLatin1String("image/")))
        return QStringLiteral("image");
    if (mimeType.startsWith(QLatin1String("video/")))
        return QStringLiteral("video");
    if (mimeType.startsWith(QLatin1String("audio/")))
        return QStringLiteral("audio");

    // Named rather than matched by prefix, because the types that belong
    // together here share no prefix worth trusting: a .docx and a .pdf have
    // nothing in common except what somebody uses them for.
    static const QStringList documents {
        QStringLiteral("application/pdf"),
        QStringLiteral("application/rtf"),
        QStringLiteral("application/epub+zip"),
        QStringLiteral("application/vnd.oasis.opendocument.text"),
        QStringLiteral("application/vnd.oasis.opendocument.spreadsheet"),
        QStringLiteral("application/vnd.oasis.opendocument.presentation"),
        QStringLiteral("application/msword"),
        QStringLiteral("application/vnd.ms-excel"),
        QStringLiteral("application/vnd.ms-powerpoint"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation"),
        QStringLiteral("text/markdown"),
        QStringLiteral("text/csv"),
    };
    if (documents.contains(mimeType))
        return QStringLiteral("document");

    static const QStringList archives {
        QStringLiteral("application/zip"),
        QStringLiteral("application/x-tar"),
        QStringLiteral("application/gzip"),
        QStringLiteral("application/x-bzip2"),
        QStringLiteral("application/x-xz"),
        QStringLiteral("application/zstd"),
        QStringLiteral("application/x-7z-compressed"),
        QStringLiteral("application/vnd.rar"),
        QStringLiteral("application/x-compressed-tar"),
    };
    if (archives.contains(mimeType))
        return QStringLiteral("archive");

    // Whatever the mime database calls a descendant of plain text, which is
    // where a Dockerfile, a makefile and every source file land -- and they are
    // code by anybody's reckoning.
    static const QMimeDatabase database;
    const QMimeType type = database.mimeTypeForName(mimeType);
    if (type.isValid() && type.allAncestors().contains(QLatin1String("text/plain")))
        return QStringLiteral("code");
    if (mimeType.startsWith(QLatin1String("text/")))
        return QStringLiteral("code");

    return {};
}

QDateTime parseWhen(const QString& text, const QDateTime& now)
{
    const QString trimmed = text.trimmed().toLower();
    if (trimmed.isEmpty())
        return {};

    if (trimmed == QLatin1String("today"))
        return QDateTime(now.date(), QTime(0, 0));
    if (trimmed == QLatin1String("yesterday"))
        return QDateTime(now.date().addDays(-1), QTime(0, 0));
    if (trimmed == QLatin1String("this week"))
        return QDateTime(now.date().addDays(-(now.date().dayOfWeek() - 1)), QTime(0, 0));
    if (trimmed == QLatin1String("this month"))
        return QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0));
    if (trimmed == QLatin1String("this year"))
        return QDateTime(QDate(now.date().year(), 1, 1), QTime(0, 0));

    // "last 7 days", ">30d", "7d", "24h", "90 minutes". The leading ">" is
    // allowed because people write it and it means the same thing here.
    static const QRegularExpression span(
        QStringLiteral("^(?:last\\s+|>\\s*|within\\s+)?([0-9]+)\\s*"
                       "(mo|months?|m|min|mins|minutes?|h|hours?|d|days?|w|weeks?|y|years?)$"));
    const QRegularExpressionMatch match = span.match(trimmed);
    if (match.hasMatch()) {
        const qint64 count = match.captured(1).toLongLong();
        const QString unit = match.captured(2);
        if (unit.startsWith(QLatin1String("mo")))
            return now.addMonths(static_cast<int>(-count));
        if (unit.startsWith(QLatin1Char('y')))
            return now.addYears(static_cast<int>(-count));
        if (unit.startsWith(QLatin1Char('w')))
            return now.addDays(-count * 7);
        if (unit.startsWith(QLatin1Char('d')))
            return now.addDays(-count);
        if (unit.startsWith(QLatin1Char('h')))
            return now.addSecs(-count * 3600);
        return now.addSecs(-count * 60); // m, min, minutes
    }

    // A plain date, which is what somebody who knows the day types.
    const QDate day = QDate::fromString(trimmed, Qt::ISODate);
    if (day.isValid())
        return QDateTime(day, QTime(0, 0));

    // Refused rather than guessed at: a date nobody can parse has to narrow
    // nothing, and the one thing it must never do is match everything.
    return {};
}

// ---------------------------------------------------------------- predicates

SearchPredicate SearchPredicate::name(const QString& text, bool caseSensitive)
{
    SearchPredicate predicate;
    predicate.field = Field::Name;
    predicate.match = Match::Contains;
    predicate.text = text;
    predicate.caseSensitive = caseSensitive;
    return predicate;
}

SearchPredicate SearchPredicate::nameGlob(const QString& pattern, bool caseSensitive)
{
    SearchPredicate predicate = name(pattern, caseSensitive);
    predicate.match = Match::Glob;
    return predicate;
}

SearchPredicate SearchPredicate::nameRegex(const QString& pattern, bool caseSensitive)
{
    SearchPredicate predicate = name(pattern, caseSensitive);
    predicate.match = Match::Regex;
    return predicate;
}

SearchPredicate SearchPredicate::pathContains(const QString& text, bool caseSensitive)
{
    SearchPredicate predicate;
    predicate.field = Field::Path;
    predicate.match = Match::Contains;
    predicate.text = text;
    predicate.caseSensitive = caseSensitive;
    predicate.cost = PredicateCost::Cheap;
    return predicate;
}

SearchPredicate SearchPredicate::extensions(const QStringList& extensions)
{
    SearchPredicate predicate;
    predicate.field = Field::Extension;
    predicate.match = Match::OneOf;
    for (const QString& one : extensions) {
        const QString cleaned = one.trimmed().toLower();
        if (!cleaned.isEmpty())
            predicate.list.append(cleaned.startsWith(QLatin1Char('.')) ? cleaned.mid(1) : cleaned);
    }
    return predicate;
}

SearchPredicate SearchPredicate::typeClasses(const QStringList& classes)
{
    SearchPredicate predicate;
    predicate.field = Field::TypeClass;
    predicate.match = Match::OneOf;
    predicate.list = classes;
    // A page of the file, because the name is a label somebody typed. See
    // ADR-0033 for why that is the answer rather than the shortcut.
    predicate.cost = PredicateCost::Metadata;
    return predicate;
}

SearchPredicate SearchPredicate::content(const QString& text, bool asRegex, bool caseSensitive)
{
    SearchPredicate predicate;
    predicate.field = Field::Content;
    predicate.match = asRegex ? Match::Regex : Match::Contains;
    predicate.text = text;
    predicate.caseSensitive = caseSensitive;
    // The dearest rung there is, so the planner leaves it until everything
    // else has narrowed the candidates: filter to the PDFs, then read them.
    predicate.cost = PredicateCost::Content;
    return predicate;
}

SearchPredicate SearchPredicate::metadataIs(const QString& key, const QString& value)
{
    SearchPredicate predicate;
    predicate.field = Field::Metadata;
    predicate.match = Match::Contains;
    predicate.list = { key };
    predicate.text = value;
    // A column on an indexed volume and a bounded read on one that was never
    // scanned. PushedDown is the claim that the index can state it; a source
    // that cannot say so demotes it, which is what the planner is for.
    predicate.cost = PredicateCost::PushedDown;
    return predicate;
}

SearchPredicate SearchPredicate::metadataAtLeast(const QString& key, double value)
{
    SearchPredicate predicate = metadataIs(key, QString());
    predicate.match = Match::AtLeast;
    predicate.number = 0;
    predicate.numberValue = value;
    return predicate;
}

SearchPredicate SearchPredicate::metadataAtMost(const QString& key, double value)
{
    SearchPredicate predicate = metadataIs(key, QString());
    predicate.match = Match::AtMost;
    predicate.number = 0;
    predicate.numberValue = value;
    return predicate;
}

SearchPredicate SearchPredicate::minSize(qint64 bytes)
{
    SearchPredicate predicate;
    predicate.field = Field::Size;
    predicate.match = Match::AtLeast;
    predicate.number = bytes;
    return predicate;
}

SearchPredicate SearchPredicate::maxSize(qint64 bytes)
{
    SearchPredicate predicate;
    predicate.field = Field::Size;
    predicate.match = Match::AtMost;
    predicate.number = bytes;
    return predicate;
}

SearchPredicate SearchPredicate::modifiedAfter(qint64 epochSeconds)
{
    return whenPredicate(Field::Modified, Match::AtLeast, epochSeconds);
}

SearchPredicate SearchPredicate::modifiedBefore(qint64 epochSeconds)
{
    return whenPredicate(Field::Modified, Match::AtMost, epochSeconds);
}

SearchPredicate SearchPredicate::createdAfter(qint64 epochSeconds)
{
    return whenPredicate(Field::Created, Match::AtLeast, epochSeconds);
}

SearchPredicate SearchPredicate::createdBefore(qint64 epochSeconds)
{
    return whenPredicate(Field::Created, Match::AtMost, epochSeconds);
}

SearchPredicate SearchPredicate::accessedAfter(qint64 epochSeconds)
{
    return whenPredicate(Field::Accessed, Match::AtLeast, epochSeconds);
}

SearchPredicate SearchPredicate::accessedBefore(qint64 epochSeconds)
{
    return whenPredicate(Field::Accessed, Match::AtMost, epochSeconds);
}

SearchPredicate SearchPredicate::kind(bool directories)
{
    SearchPredicate predicate;
    predicate.field = Field::Kind;
    predicate.match = Match::Equals;
    predicate.flag = directories;
    return predicate;
}

SearchPredicate SearchPredicate::hidden(bool hidden)
{
    SearchPredicate predicate;
    predicate.field = Field::Hidden;
    predicate.match = Match::Equals;
    predicate.flag = hidden;
    predicate.cost = PredicateCost::Cheap;
    return predicate;
}

SearchPredicate SearchPredicate::underPath(const QString& uri)
{
    SearchPredicate predicate;
    predicate.field = Field::Under;
    predicate.match = Match::Contains;
    predicate.text = uri;
    predicate.cost = PredicateCost::Cheap;
    return predicate;
}

bool SearchPredicate::needsFile() const
{
    return field == Field::TypeClass || field == Field::Content || field == Field::Metadata;
}

bool SearchPredicate::matches(const FileEntry& entry, const SearchIo& io, ContentMatch* whyOut) const
{
    const bool hit = [&] {
        switch (field) {
        case Field::Name:
            return text.isEmpty() || matchesPattern(entry.name, *this);
        case Field::Path:
            return containsText(entry.uri.toString(), text, caseSensitive, wholeWord);
        case Field::Extension:
            return list.isEmpty() || list.contains(entry.uri.suffix());
        case Field::Size:
            // Directories are compared too, on the size the listing gave them,
            // which is the entry's own rather than what is under it. That is
            // what both engines have always done.
            return inRange(entry.size, *this);
        case Field::Modified:
            return timeMatches(entry.modified, *this);
        case Field::Created:
            return timeMatches(entry.created, *this);
        case Field::Accessed:
            return timeMatches(entry.accessed, *this);
        case Field::Kind:
            return entry.isDir == flag;
        case Field::Hidden:
            return entry.isHidden == flag;
        case Field::TypeClass: {
            if (list.isEmpty())
                return true;
            // A folder is a class of its own, and no amount of reading one
            // would say so.
            if (entry.isDir)
                return list.contains(QStringLiteral("folder"));
            if (!io)
                return false;
            return list.contains(classOfMimeType(
                FileType::identify(entry.name, io.read(entry.uri, 0, FileType::kSampleBytes))));
        }
        case Field::Content: {
            if (text.isEmpty())
                return true;
            if (!io)
                return false;
            const ContentMatch found = findInContents(entry, *this, io);
            if (found.isValid() && whyOut)
                *whyOut = found;
            return found.isValid();
        }
        case Field::Metadata: {
            if (list.isEmpty() || !io.facts)
                return false;
            const QString wanted = list.first();
            for (const SearchFact& fact : io.facts(entry)) {
                if (fact.key != wanted)
                    continue;
                if (match == Match::AtLeast)
                    return fact.hasNumber && fact.number >= numberValue;
                if (match == Match::AtMost)
                    return fact.hasNumber && fact.number <= numberValue;
                if (containsText(fact.text, text, caseSensitive, wholeWord))
                    return true;
            }
            return false;
        }
        case Field::Under:
            return isUnder(entry.uri.toString(), text);
        }
        return true;
    }();

    return negate ? !hit : hit;
}

ContentMatch findInContents(
    const FileEntry& entry, const SearchPredicate& predicate, const SearchIo& io, ContentSkip* skippedOut)
{
    const auto give = [skippedOut](ContentSkip why) {
        if (skippedOut)
            *skippedOut = why;
        return ContentMatch {};
    };

    if (skippedOut)
        *skippedOut = ContentSkip::None;
    if (entry.isDir || predicate.text.isEmpty() || !io)
        return give(ContentSkip::None);
    if (entry.size > io.ceiling)
        return give(ContentSkip::TooBig);

    const QByteArray head = io.read(entry.uri, 0, FileType::kSampleBytes);
    if (head.isEmpty() && entry.size > 0)
        return give(ContentSkip::Unreadable);
    // Decided by what is in the file, never by its suffix: a suffix list is
    // wrong about the files people actually have, and the sniffer is already
    // answering exactly this question for the preview layer.
    if (!predicate.includeBinary && !FileType::looksLikeText(head))
        return give(ContentSkip::Binary);

    // The byte order mark decides the encoding, and a decoder is kept across
    // windows so a character split by one is not mangled by the next.
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    if (head.startsWith(QByteArrayLiteral("\xff\xfe")) || head.startsWith(QByteArrayLiteral("\xfe\xff")))
        decoder = QStringDecoder(QStringDecoder::Utf16);
    else if (head.startsWith(QByteArrayLiteral("\xef\xbb\xbf")))
        decoder = QStringDecoder(QStringDecoder::Utf8);

    const auto options = predicate.caseSensitive ? QRegularExpression::NoPatternOption
                                                 : QRegularExpression::CaseInsensitiveOption;
    QRegularExpression pattern(predicate.match == SearchPredicate::Match::Regex
            ? predicate.text
            : QRegularExpression::escape(predicate.text),
        options);
    if (predicate.wholeWord) {
        pattern.setPattern(QStringLiteral("(?<![\\p{L}\\p{N}])%1(?![\\p{L}\\p{N}])").arg(pattern.pattern()));
    }
    if (!pattern.isValid())
        return give(ContentSkip::None);

    // Windows rather than the whole file, the same rule the preview layer
    // follows, with the tail of one window carried in front of the next so a
    // match lying across the boundary is still found.
    QString carried;
    qint64 offset = 0;
    int linesBefore = 0;
    const qint64 ceiling = qMin(io.ceiling, entry.size > 0 ? entry.size : io.ceiling);

    while (offset < ceiling) {
        if (io.cancelled && io.cancelled())
            return give(ContentSkip::None);

        const QByteArray window = io.read(entry.uri, offset, SearchIo::kWindowBytes);
        if (window.isEmpty())
            break;
        offset += window.size();

        const QString text = carried + QString(decoder.decode(window));
        if (decoder.hasError())
            return give(ContentSkip::Undecodable);

        const QRegularExpressionMatch found = pattern.match(text);
        if (found.hasMatch()) {
            const qsizetype at = found.capturedStart();
            const qsizetype lineStart = text.lastIndexOf(QLatin1Char('\n'), at) + 1;
            qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), at);
            if (lineEnd < 0)
                lineEnd = text.size();

            const QString whole = text.mid(lineStart, lineEnd - lineStart);
            // The line is trimmed for showing, so the column has to move with
            // it -- a position counted from an indent nobody can see would
            // mark the wrong characters.
            qsizetype indent = 0;
            while (indent < whole.size() && whole.at(indent).isSpace())
                ++indent;

            ContentMatch match;
            match.line = whole.trimmed();
            match.lineNumber
                = linesBefore + static_cast<int>(QStringView(text).left(at).count(QLatin1Char('\n'))) + 1;
            match.column = static_cast<int>(qMax<qsizetype>(0, at - lineStart - indent));
            match.length = static_cast<int>(found.capturedLength());
            return match;
        }

        if (offset >= ceiling || window.size() < SearchIo::kWindowBytes)
            break;

        // What is carried forward, and the lines that are not.
        const qsizetype keep = qMin<qsizetype>(SearchIo::kOverlapBytes, text.size());
        const QString tail = text.right(keep);
        linesBefore += static_cast<int>(QStringView(text).left(text.size() - keep).count(QLatin1Char('\n')));
        carried = tail;
    }

    if (entry.size > ceiling)
        return give(ContentSkip::TooBig);
    return give(ContentSkip::None);
}

// ----------------------------------------------------------------- the query

// ---- written down -----------------------------------------------------------

namespace {

    /// The stored name of every Field, in one table read both ways.
    ///
    /// One table rather than two switch statements, because two of those drift:
    /// the day somebody adds a value to Field and only one of them knows about
    /// it, a query saves under a name nothing can read back. See MOLE-163.
    struct FieldName
    {
        SearchPredicate::Field field;
        const char* name;
    };

    const QList<FieldName>& fieldNames()
    {
        using Field = SearchPredicate::Field;
        static const QList<FieldName> names {
            { Field::Name, "name" },
            { Field::Path, "path" },
            { Field::Extension, "extension" },
            { Field::Size, "size" },
            { Field::Modified, "modified" },
            { Field::Created, "created" },
            { Field::Accessed, "accessed" },
            { Field::Kind, "kind" },
            { Field::Hidden, "hidden" },
            { Field::TypeClass, "typeClass" },
            { Field::Content, "content" },
            { Field::Metadata, "metadata" },
            { Field::Under, "under" },
        };
        return names;
    }

    struct MatchName
    {
        SearchPredicate::Match match;
        const char* name;
    };

    const QList<MatchName>& matchNames()
    {
        using Match = SearchPredicate::Match;
        static const QList<MatchName> names {
            { Match::Contains, "contains" },
            { Match::Equals, "equals" },
            { Match::AtLeast, "atLeast" },
            { Match::AtMost, "atMost" },
            { Match::Glob, "glob" },
            { Match::Regex, "regex" },
            { Match::OneOf, "oneOf" },
        };
        return names;
    }

    /// A flag that is absent takes its default, and one that is there is what it
    /// says. The four that are usually false are exactly the ones a hand-written
    /// object leaves out.
    bool flagOf(const QJsonObject& json, const char* key, bool fallback = false)
    {
        const QJsonValue value = json.value(QLatin1String(key));
        return value.isBool() ? value.toBool() : fallback;
    }

} // namespace

PredicateCost SearchPredicate::costOf(Field field)
{
    switch (field) {
    case Field::Path:
    case Field::Hidden:
    case Field::Under:
        // Answerable from the entry a listing already handed over, and not
        // from any source's own query: a uri prefix and a hidden flag are
        // ours to check.
        return PredicateCost::Cheap;
    case Field::TypeClass:
        // What is *in* the file decides its class, so an unscanned volume
        // pays for a bounded read.
        return PredicateCost::Metadata;
    case Field::Content:
        return PredicateCost::Content;
    case Field::Name:
    case Field::Extension:
    case Field::Size:
    case Field::Modified:
    case Field::Created:
    case Field::Accessed:
    case Field::Kind:
    case Field::Metadata:
        // The claim that a source can state it in its own query. A source
        // that cannot say so demotes it, which is what the planner is for.
        return PredicateCost::PushedDown;
    }
    return PredicateCost::Cheap;
}

QString SearchPredicate::fieldName(Field field)
{
    for (const FieldName& entry : fieldNames()) {
        if (entry.field == field)
            return QString::fromLatin1(entry.name);
    }
    // Unreachable while the table covers the enum, and a suite holds it to that.
    return {};
}

std::optional<SearchPredicate::Field> SearchPredicate::fieldFromName(const QString& name)
{
    for (const FieldName& entry : fieldNames()) {
        if (QLatin1String(entry.name) == name)
            return entry.field;
    }
    return std::nullopt;
}

QString SearchPredicate::matchName(Match match)
{
    for (const MatchName& entry : matchNames()) {
        if (entry.match == match)
            return QString::fromLatin1(entry.name);
    }
    return {};
}

std::optional<SearchPredicate::Match> SearchPredicate::matchFromName(const QString& name)
{
    for (const MatchName& entry : matchNames()) {
        if (QLatin1String(entry.name) == name)
            return entry.match;
    }
    return std::nullopt;
}

QJsonObject SearchPredicate::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("field")] = fieldName(field);
    json[QStringLiteral("match")] = matchName(match);
    if (!text.isEmpty())
        json[QStringLiteral("text")] = text;
    if (!list.isEmpty())
        json[QStringLiteral("list")] = QJsonArray::fromStringList(list);
    // The numbers are written when they say something. A size of nought and an
    // epoch of nought are what a criterion looks like before anybody filled it
    // in, and an object with every member in it is one nobody can read.
    if (number != 0)
        json[QStringLiteral("number")] = number;
    if (numberValue != 0)
        json[QStringLiteral("numberValue")] = numberValue;
    // The four that are usually false, and are the whole of what somebody means
    // when they set them: written only when true, read as false when absent.
    if (flag)
        json[QStringLiteral("flag")] = true;
    if (caseSensitive)
        json[QStringLiteral("caseSensitive")] = true;
    if (wholeWord)
        json[QStringLiteral("wholeWord")] = true;
    if (negate)
        json[QStringLiteral("negate")] = true;
    if (includeBinary)
        json[QStringLiteral("includeBinary")] = true;
    // `cost` is deliberately absent -- see the note in the header.
    return json;
}

std::optional<SearchPredicate> SearchPredicate::fromJson(const QJsonObject& json)
{
    SearchPredicate predicate;

    // Absent takes the default; present and unreadable is a refusal. That is the
    // whole distinction: a query written by an older build is missing keys, and a
    // query written by a newer one names things this build has never heard of.
    const QJsonValue fieldValue = json.value(QStringLiteral("field"));
    if (!fieldValue.isUndefined()) {
        const std::optional<Field> read = fieldFromName(fieldValue.toString());
        if (!read)
            return std::nullopt;
        predicate.field = *read;
    }
    const QJsonValue matchValue = json.value(QStringLiteral("match"));
    if (!matchValue.isUndefined()) {
        const std::optional<Match> read = matchFromName(matchValue.toString());
        if (!read)
            return std::nullopt;
        predicate.match = *read;
    }

    predicate.text = json.value(QStringLiteral("text")).toString();
    const QJsonArray list = json.value(QStringLiteral("list")).toArray();
    for (const QJsonValue& value : list) {
        if (value.isString())
            predicate.list.append(value.toString());
    }
    predicate.number = json.value(QStringLiteral("number")).toInteger(0);
    predicate.numberValue = json.value(QStringLiteral("numberValue")).toDouble(0);
    predicate.flag = flagOf(json, "flag");
    predicate.caseSensitive = flagOf(json, "caseSensitive");
    predicate.wholeWord = flagOf(json, "wholeWord");
    predicate.negate = flagOf(json, "negate");
    predicate.includeBinary = flagOf(json, "includeBinary");
    // And `cost` is derived rather than read. Not stored, because it is the
    // planner's answer and depends on where the search runs -- and not left at
    // its default either, because a content search that arrives claiming to be
    // pushed down is one a source will answer without ever opening a file.
    predicate.cost = costOf(predicate.field);
    return predicate;
}

QJsonObject SearchQuery::toJson() const
{
    QJsonObject json;
    QJsonArray stored;
    for (const SearchPredicate& predicate : predicates)
        stored.append(predicate.toJson());
    json[QStringLiteral("predicates")] = stored;
    if (volumeId != -1)
        json[QStringLiteral("volumeId")] = volumeId;
    json[QStringLiteral("limit")] = limit;
    if (!excluded.isEmpty())
        json[QStringLiteral("excluded")] = QJsonArray::fromStringList(excluded);
    if (maxDepth != -1)
        json[QStringLiteral("maxDepth")] = maxDepth;
    return json;
}

std::optional<SearchQuery> SearchQuery::fromJson(const QJsonObject& json)
{
    SearchQuery query;
    const QJsonArray stored = json.value(QStringLiteral("predicates")).toArray();
    for (const QJsonValue& value : stored) {
        const std::optional<SearchPredicate> predicate = SearchPredicate::fromJson(value.toObject());
        // One criterion this build cannot read refuses the whole query. A query
        // that came back with three of its four criteria matches more files than
        // whoever saved it asked for, and a chain that deletes what it finds is
        // exactly where that must not happen quietly.
        if (!predicate)
            return std::nullopt;
        query.predicates.append(*predicate);
    }
    query.volumeId = json.value(QStringLiteral("volumeId")).toInteger(-1);
    query.limit = json.value(QStringLiteral("limit")).toInt(query.limit);
    const QJsonArray excluded = json.value(QStringLiteral("excluded")).toArray();
    for (const QJsonValue& value : excluded) {
        if (value.isString())
            query.excluded.append(value.toString());
    }
    query.maxDepth = json.value(QStringLiteral("maxDepth")).toInt(-1);
    return query;
}

SearchQuery& SearchQuery::add(SearchPredicate predicate)
{
    predicates.append(std::move(predicate));
    return *this;
}

SearchQuery& SearchQuery::addIfSet(const SearchPredicate& predicate)
{
    switch (predicate.field) {
    case SearchPredicate::Field::Name:
    case SearchPredicate::Field::Path:
    case SearchPredicate::Field::Under:
        if (predicate.text.isEmpty())
            return *this;
        break;
    case SearchPredicate::Field::Extension:
    case SearchPredicate::Field::TypeClass:
        if (predicate.list.isEmpty())
            return *this;
        break;
    case SearchPredicate::Field::Content:
        if (predicate.text.isEmpty())
            return *this;
        break;
    case SearchPredicate::Field::Metadata:
        if (predicate.list.isEmpty()
            || (predicate.match == SearchPredicate::Match::Contains && predicate.text.isEmpty())) {
            return *this;
        }
        break;
    case SearchPredicate::Field::Size:
    case SearchPredicate::Field::Modified:
    case SearchPredicate::Field::Created:
    case SearchPredicate::Field::Accessed:
        // Minus one is how the form says "no limit", and a limit of nought
        // bytes or of the epoch would quietly mean something else.
        if (predicate.number < 0)
            return *this;
        break;
    case SearchPredicate::Field::Kind:
    case SearchPredicate::Field::Hidden:
        break;
    }
    return add(predicate);
}

bool SearchQuery::isExcluded(const QString& name) const
{
    for (const QString& pattern : excluded) {
        const QString trimmed = pattern.trimmed();
        if (trimmed.isEmpty())
            continue;
        const QRegularExpression glob(
            QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(
                trimmed, QRegularExpression::UnanchoredWildcardConversion)));
        if (glob.match(name).hasMatch())
            return true;
    }
    return false;
}

// ------------------------------------------------------------------ the plan

SearchPlan::SearchPlan(QList<SearchPredicate> pushedDown, QList<SearchPredicate> remainder)
    : m_pushedDown(std::move(pushedDown))
    , m_remainder(std::move(remainder))
{
}

bool SearchPlan::needsFile() const
{
    return std::any_of(m_remainder.cbegin(), m_remainder.cend(),
        [](const SearchPredicate& predicate) { return predicate.needsFile(); });
}

bool SearchPlan::readsWholeFiles() const
{
    return std::any_of(m_remainder.cbegin(), m_remainder.cend(),
        [](const SearchPredicate& predicate) { return predicate.readsWholeFile(); });
}

bool SearchPlan::matches(const FileEntry& entry, const SearchIo& io, ContentMatch* whyOut) const
{
    return matchesWithoutFile(entry) && matchesNeedingFile(entry, io, whyOut);
}

bool SearchPlan::matchesWithoutFile(const FileEntry& entry) const
{
    for (const SearchPredicate& predicate : m_remainder) {
        if (predicate.needsFile())
            continue;
        if (!predicate.matches(entry))
            return false;
    }
    return true;
}

bool SearchPlan::matchesNeedingFile(const FileEntry& entry, const SearchIo& io, ContentMatch* whyOut) const
{
    for (const SearchPredicate& predicate : m_remainder) {
        if (!predicate.needsFile())
            continue;
        if (!predicate.matches(entry, io, whyOut))
            return false;
    }
    return true;
}

SearchPlan planSearch(const SearchQuery& query, SearchSource source)
{
    QList<SearchPredicate> pushedDown;
    QList<SearchPredicate> remainder;

    for (const SearchPredicate& predicate : query.predicates) {
        // A walk has no query of its own to push anything into: it lists a
        // directory and looks at what came back, so every criterion is one it
        // evaluates. Saying so plainly beats a second path that happens to
        // reach the same evaluator by a different name.
        const bool pushed = source == SearchSource::Index && indexCanExpress(predicate);
        (pushed ? pushedDown : remainder).append(predicate);
    }

    // Stable, so two criteria of the same cost stay in the order they were
    // written. Somebody who puts the narrow one first is telling you something,
    // and a sort that reshuffles equals throws that away.
    std::stable_sort(
        remainder.begin(), remainder.end(), [](const SearchPredicate& left, const SearchPredicate& right) {
            return evaluationCost(left) < evaluationCost(right);
        });

    return { std::move(pushedDown), std::move(remainder) };
}

} // namespace mole
