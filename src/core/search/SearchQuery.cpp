#include "core/search/SearchQuery.h"

#include "core/data/FileType.h"

#include <QMimeDatabase>
#include <QRegularExpression>

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
        case SearchPredicate::Field::Hidden:
        case SearchPredicate::Field::TypeClass:
        case SearchPredicate::Field::Path:
        case SearchPredicate::Field::Under:
            // Rows hold the path inside their volume rather than the whole uri,
            // nothing records what a file is, and nothing records hidden.
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

bool SearchPredicate::needsSample() const
{
    return field == Field::TypeClass;
}

bool SearchPredicate::matches(const FileEntry& entry, const SampleReader& sample) const
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
            if (!sample)
                return false;
            return list.contains(classOfMimeType(FileType::identify(entry.name, sample(entry.uri))));
        }
        case Field::Under:
            return isUnder(entry.uri.toString(), text);
        }
        return true;
    }();

    return negate ? !hit : hit;
}

// ----------------------------------------------------------------- the query

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

bool SearchPlan::needsSample() const
{
    return std::any_of(m_remainder.cbegin(), m_remainder.cend(),
        [](const SearchPredicate& predicate) { return predicate.needsSample(); });
}

bool SearchPlan::matches(const FileEntry& entry, const SampleReader& sample) const
{
    for (const SearchPredicate& predicate : m_remainder) {
        if (!predicate.matches(entry, sample))
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
