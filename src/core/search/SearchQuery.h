#pragma once

#include "core/vfs/FileEntry.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

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

/// The first page of a file, for the criteria an entry cannot answer.
///
/// Empty when it could not be read, which is not the same as a file that does
/// not match -- see SearchPredicate::matches().
using SampleReader = std::function<QByteArray(const VfsUri&)>;

/// One criterion of a search.
///
/// A predicate rather than a field on a struct of options, because the set has
/// grown and will keep growing, and because the planner has to reason about
/// them one at a time. A struct of optionals cannot say *this one is expensive,
/// do it last*.
struct SearchPredicate
{
    enum class Field {
        Name, ///< the file's name
        Path, ///< the whole uri
        Extension, ///< one of a list, lowercased, no dot
        Size, ///< bytes
        Modified, ///< seconds since the epoch
        Created, ///< the same, where the drive reports it
        Accessed, ///< the same, where the drive reports it
        Kind, ///< a directory, or a file
        Hidden, ///< what the drive calls hidden
        /// What sort of file it is, from what is in it rather than what it is
        /// called: images, video, audio, documents, archives, code, folders.
        TypeClass,
        Under, ///< the entry sits under this uri
    };

    enum class Match {
        Contains, ///< a substring, folded unless caseSensitive
        Equals,
        AtLeast,
        AtMost,
        Glob, ///< report-*.pdf
        Regex, ///< anchored nowhere; what the user typed
        OneOf, ///< `text` holds a comma-separated list
    };

    Field field = Field::Name;
    Match match = Match::Contains;
    QString text;
    QStringList list; ///< Extension, TypeClass
    qint64 number = 0; ///< Size, Modified, Created, Accessed
    bool flag = false; ///< Kind: a directory. Hidden: hidden.
    bool caseSensitive = false; ///< Name, Path
    /// Whole words only, for a name or a path matched as a substring. "report"
    /// stops matching "reporting", which is what somebody looking for a
    /// document called Report means and never gets from a substring.
    bool wholeWord = false;
    /// Everything this would have matched, and nothing it would. The one
    /// negation there is: *not* on a name or a path does most of what a general
    /// boolean builder would, and a general boolean builder is a different
    /// feature.
    bool negate = false;

    /// What answering this costs.
    ///
    /// A member rather than a property of the field, because the same criterion
    /// costs different things in different places: a camera model is a column
    /// on an indexed volume and a read of the header on one that was never
    /// scanned. The planner reads this; it does not set it.
    PredicateCost cost = PredicateCost::PushedDown;

    // ---- the criteria there are ------------------------------------------

    static SearchPredicate name(const QString& text, bool caseSensitive = false);
    static SearchPredicate nameGlob(const QString& pattern, bool caseSensitive = false);
    static SearchPredicate nameRegex(const QString& pattern, bool caseSensitive = false);
    /// Anywhere in the uri, which is what somebody who remembers the folder and
    /// not the file is asking about.
    static SearchPredicate pathContains(const QString& text, bool caseSensitive = false);
    /// Any of these extensions. A list because it never should have been one.
    static SearchPredicate extensions(const QStringList& extensions);
    /// Any of these classes -- "image", "video", "audio", "document",
    /// "archive", "code", "folder".
    static SearchPredicate typeClasses(const QStringList& classes);
    static SearchPredicate minSize(qint64 bytes);
    static SearchPredicate maxSize(qint64 bytes);
    static SearchPredicate modifiedAfter(qint64 epochSeconds);
    static SearchPredicate modifiedBefore(qint64 epochSeconds);
    static SearchPredicate createdAfter(qint64 epochSeconds);
    static SearchPredicate createdBefore(qint64 epochSeconds);
    static SearchPredicate accessedAfter(qint64 epochSeconds);
    static SearchPredicate accessedBefore(qint64 epochSeconds);
    /// Only directories, or only files. Both kinds are wanted when neither is
    /// asked for, and neither is wanted when both are -- which is what asking
    /// for a directory that is also a file has to mean.
    static SearchPredicate kind(bool directories);
    static SearchPredicate hidden(bool hidden);
    /// The entry's uri begins with this. What *search inside this folder* means
    /// to a source whose rows cover a whole volume.
    static SearchPredicate underPath(const QString& uri);

    /// Whether `entry` satisfies this.
    ///
    /// Pure for everything a listing already answered. `sample` supplies the
    /// first page of the file for the criteria that need what is inside it; a
    /// predicate that needs one and is given none **does not match**, because
    /// the alternative is quietly answering a question nobody asked.
    [[nodiscard]] bool matches(const FileEntry& entry, const SampleReader& sample = {}) const;
    /// True when answering this needs the file rather than the listing.
    [[nodiscard]] bool needsSample() const;
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

    /// Directory names a walk must not descend into, as globs: `node_modules`,
    /// `.git`, `build`.
    ///
    /// Not a predicate, because a predicate says what to keep and this says
    /// where not to go. Nothing else makes a search of a developer's disk
    /// usable, and the difference shows in directories entered rather than in
    /// results returned.
    QStringList excluded;

    /// How far down to go. -1 is everything; 0 is the folder itself and nothing
    /// under it, which is how *just here* is asked for.
    int maxDepth = -1;

    SearchQuery& add(SearchPredicate predicate);
    /// Adds it only when there is something to ask -- an empty name or an
    /// absent size limit is not a criterion, it is a criterion left blank.
    SearchQuery& addIfSet(const SearchPredicate& predicate);

    /// Whether a directory called `name` is one the walk should not enter.
    [[nodiscard]] bool isExcluded(const QString& name) const;
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

    /// True when something left over needs the file itself, so a source that
    /// cannot read one is going to answer short.
    [[nodiscard]] bool needsSample() const;

    /// Whether `entry` satisfies every criterion left over.
    ///
    /// Cheapest first, so the expensive ones are only reached by what survived
    /// everything else: filter to the PDFs, then open them.
    [[nodiscard]] bool matches(const FileEntry& entry, const SampleReader& sample = {}) const;

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

/// What sort of file a MIME type is: "image", "video", "audio", "document",
/// "archive", "code", or empty for anything that is none of them.
///
/// Extensions are how a file is stored; a class is how anybody thinks about it.
/// The mapping is here rather than in the form so that the answer is the same
/// wherever it is asked.
[[nodiscard]] QString classOfMimeType(const QString& mimeType);

/// Every class a search can ask for, in the order a form should offer them.
[[nodiscard]] QStringList knownTypeClasses();

/// Turns what somebody typed about time into a moment.
///
/// `today`, `yesterday`, `this week`, `this month`, `this year`, `last 7 days`,
/// `>30d`, `7d`, `24h`, or a plain `2026-08-11`. Everything else is refused
/// with an invalid QDateTime, because a date nobody can parse must narrow
/// nothing rather than match everything.
///
/// The precedent is `LiveSearchController::parseSize()`: a form should not make
/// somebody count zeros, and it should not make them work out a date either.
[[nodiscard]] QDateTime parseWhen(const QString& text, const QDateTime& now);

} // namespace mole
