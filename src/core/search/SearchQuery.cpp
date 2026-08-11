#include "core/search/SearchQuery.h"

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
        switch (predicate.field) {
        case SearchPredicate::Field::Name:
        case SearchPredicate::Field::Extension:
        case SearchPredicate::Field::Size:
        case SearchPredicate::Field::Kind:
            // A column apiece, each with an index on it.
            return predicate.cost == PredicateCost::PushedDown;
        case SearchPredicate::Field::Modified:
            // The column is there and no clause reads it yet. It arrives with
            // the form that asks for dates, and until then this is evaluated
            // rather than ignored -- which is the whole point of saying so.
            return false;
        case SearchPredicate::Field::Path:
            // Rows hold the path inside their volume, not the whole uri, so the
            // prefix somebody typed is not a prefix of anything stored. This is
            // what the indexed answer has always been narrowed by afterwards;
            // now it is written down instead of done by hand at the caller.
            return false;
        }
        return false;
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

SearchPredicate SearchPredicate::name(const QString& text, bool caseSensitive)
{
    SearchPredicate predicate;
    predicate.field = Field::Name;
    predicate.match = Match::Contains;
    predicate.text = text;
    predicate.caseSensitive = caseSensitive;
    return predicate;
}

SearchPredicate SearchPredicate::extension(const QString& extension)
{
    SearchPredicate predicate;
    predicate.field = Field::Extension;
    predicate.match = Match::Equals;
    predicate.text = extension.toLower();
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
    SearchPredicate predicate;
    predicate.field = Field::Modified;
    predicate.match = Match::AtLeast;
    predicate.number = epochSeconds;
    return predicate;
}

SearchPredicate SearchPredicate::modifiedBefore(qint64 epochSeconds)
{
    SearchPredicate predicate;
    predicate.field = Field::Modified;
    predicate.match = Match::AtMost;
    predicate.number = epochSeconds;
    return predicate;
}

SearchPredicate SearchPredicate::kind(bool directories)
{
    SearchPredicate predicate;
    predicate.field = Field::Kind;
    predicate.match = Match::Equals;
    predicate.flag = directories;
    return predicate;
}

SearchPredicate SearchPredicate::underPath(const QString& uri)
{
    SearchPredicate predicate;
    predicate.field = Field::Path;
    predicate.match = Match::StartsWith;
    predicate.text = uri;
    // Nothing but a string comparison on what the listing already said.
    predicate.cost = PredicateCost::Cheap;
    return predicate;
}

bool SearchPredicate::matches(const FileEntry& entry) const
{
    switch (field) {
    case Field::Name:
        if (text.isEmpty())
            return true;
        return caseSensitive ? entry.name.contains(text, Qt::CaseSensitive)
                             : foldForSearch(entry.name).contains(foldForSearch(text));
    case Field::Extension:
        return text.isEmpty() || entry.uri.suffix() == text;
    case Field::Size:
        // Directories are compared too, on the size the listing gave them,
        // which is the entry's own rather than what is under it. That is what
        // both engines have always done, and changing it is a decision for
        // whoever adds the form field, not for the merge.
        return match == Match::AtMost ? entry.size <= number : entry.size >= number;
    case Field::Modified: {
        if (!entry.modified.isValid())
            return false;
        const qint64 when = entry.modified.toSecsSinceEpoch();
        return match == Match::AtMost ? when <= number : when >= number;
    }
    case Field::Kind:
        return entry.isDir == flag;
    case Field::Path:
        return isUnder(entry.uri.toString(), text);
    }
    return true;
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
    case SearchPredicate::Field::Extension:
    case SearchPredicate::Field::Path:
        if (predicate.text.isEmpty())
            return *this;
        break;
    case SearchPredicate::Field::Size:
    case SearchPredicate::Field::Modified:
        // Minus one is how the form says "no limit", and a limit of nought
        // bytes would quietly match nothing at all.
        if (predicate.number < 0)
            return *this;
        break;
    case SearchPredicate::Field::Kind:
        break;
    }
    return add(predicate);
}

SearchPlan::SearchPlan(QList<SearchPredicate> pushedDown, QList<SearchPredicate> remainder)
    : m_pushedDown(std::move(pushedDown))
    , m_remainder(std::move(remainder))
{
}

bool SearchPlan::matches(const FileEntry& entry) const
{
    for (const SearchPredicate& predicate : m_remainder) {
        if (!predicate.matches(entry))
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
