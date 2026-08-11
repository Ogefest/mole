#pragma once

#include "core/vfs/FileEntry.h"

#include <QList>
#include <QString>

namespace mole {

/// What answering one criterion costs.
///
/// The order is the order of evaluation: everything a source can answer in its
/// own query, then everything answerable from the listing it already returned,
/// then what needs a small read, then what needs the file. That ladder is what
/// makes *PDFs containing "invoice"* open only the PDFs.
enum class PredicateCost {
    PushedDown, ///< the index's WHERE clause, or the walker's own filter
    Cheap, ///< answerable from the FileEntry the listing already returned
    Metadata, ///< the index when it holds it, otherwise a bounded read
    Content, ///< a read of the file, always
};

/// One criterion of a search.
///
/// A predicate rather than a field on a struct of options, because the set is
/// about to grow -- a date range, a type class, a metadata field, a content
/// match -- and because the planner has to reason about them one at a time. A
/// struct of optionals cannot say *this one is expensive, do it last*.
struct SearchPredicate
{
    enum class Field {
        Name, ///< a substring of the file's name
        Extension, ///< exact, lowercased, no dot
        Size, ///< bytes
        Modified, ///< seconds since the epoch
        Kind, ///< a directory, or a file
        Path, ///< the entry sits under this uri
    };

    enum class Match {
        Contains,
        Equals,
        AtLeast,
        AtMost,
        StartsWith,
    };

    Field field = Field::Name;
    Match match = Match::Contains;
    QString text; ///< Name, Extension, Path
    qint64 number = 0; ///< Size, Modified
    bool flag = false; ///< Kind: true means a directory
    bool caseSensitive = false; ///< Name

    /// What answering this costs.
    ///
    /// A member rather than a property of the field, because the same criterion
    /// costs different things in different places: a camera model is a column
    /// on an indexed volume and a read of the header on one that was never
    /// scanned. The planner reads this; it does not set it.
    PredicateCost cost = PredicateCost::PushedDown;

    // ---- the criteria there are ------------------------------------------

    static SearchPredicate name(const QString& text, bool caseSensitive = false);
    static SearchPredicate extension(const QString& extension);
    static SearchPredicate minSize(qint64 bytes);
    static SearchPredicate maxSize(qint64 bytes);
    static SearchPredicate modifiedAfter(qint64 epochSeconds);
    static SearchPredicate modifiedBefore(qint64 epochSeconds);
    /// Only directories, or only files. Both kinds are wanted when neither is
    /// asked for, and neither is wanted when both are -- which is what asking
    /// for a directory that is also a file has to mean.
    static SearchPredicate kind(bool directories);
    /// The entry's uri begins with this. What *search inside this folder* means
    /// to a source whose rows cover a whole volume.
    static SearchPredicate underPath(const QString& uri);

    /// Whether `entry` satisfies this.
    ///
    /// Pure: no I/O, no state, nothing that can differ between two calls. Every
    /// criterion the search grows proves itself here, once, rather than twice
    /// in two languages.
    [[nodiscard]] bool matches(const FileEntry& entry) const;
};

/// Everything one search asks for.
///
/// One type, given to both engines. Two of these -- one per engine, kept in
/// step by hand -- is what ADR-0005 warned the cost of would be: two paths for
/// every criterion, and a third place deciding which of them can answer it.
struct SearchQuery
{
    QList<SearchPredicate> predicates;

    /// Which indexed volume to ask, or -1 for every one of them. Nothing to a
    /// walk, which is scoped by the root it is handed.
    qint64 volumeId = -1;

    /// How many hits are wanted before a search stops looking. Both engines
    /// honour it, and a search that stops here has to say so rather than let
    /// the list read as complete.
    int limit = 10000;

    SearchQuery& add(SearchPredicate predicate);
    /// Adds it only when there is something to ask -- an empty name or an
    /// absent size limit is not a criterion, it is a criterion left blank.
    SearchQuery& addIfSet(const SearchPredicate& predicate);
};

/// Which engine a plan is for.
enum class SearchSource {
    Index, ///< SQL over the rows a scan wrote
    Walk, ///< a live walk, filtering entries as the listing hands them over
};

/// What a source will answer for itself, and what is left over.
class SearchPlan
{
public:
    SearchPlan(QList<SearchPredicate> pushedDown, QList<SearchPredicate> remainder);

    /// The criteria the source's own query applies.
    [[nodiscard]] const QList<SearchPredicate>& pushedDown() const { return m_pushedDown; }

    /// The criteria it cannot, cheapest first, for whoever has the entries.
    ///
    /// Also the answer to *what did this source have to be helped with* — the
    /// status line owes the user that, and a form greying a field owes them the
    /// reason.
    [[nodiscard]] const QList<SearchPredicate>& remainder() const { return m_remainder; }

    /// True when the source answered the whole question by itself.
    [[nodiscard]] bool pushedDownEverything() const { return m_remainder.isEmpty(); }

    /// Whether `entry` satisfies every criterion left over. A source applies
    /// this to what its own query returned.
    [[nodiscard]] bool matches(const FileEntry& entry) const;

private:
    QList<SearchPredicate> m_pushedDown;
    QList<SearchPredicate> m_remainder;
};

/// Splits `query` into what `source` can express and what it cannot.
///
/// Nothing is ever dropped: a criterion a source cannot push down is evaluated
/// afterwards instead, which is the difference between a slower answer and a
/// wrong one. The leftovers come back cheapest first, and within one cost in
/// the order they were written -- somebody who puts the narrow one first is
/// telling you something.
[[nodiscard]] SearchPlan planSearch(const SearchQuery& query, SearchSource source);

/// Whether `uri` is `prefix` or sits under it.
///
/// A plain prefix test is wrong and quietly so: "mem:///database" begins with
/// "mem:///data" and is not inside it. The boundary has to be a separator.
[[nodiscard]] bool isUnder(const QString& uri, const QString& prefix);

/// The lowercase form names are matched in.
///
/// SQLite's LIKE and NOCASE only fold ASCII, so "Łódź" would never match
/// "łódź". Both engines fold through this, so a name means the same thing to
/// each of them.
[[nodiscard]] QString foldForSearch(const QString& text);

} // namespace mole
