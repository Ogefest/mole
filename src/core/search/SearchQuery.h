#pragma once

#include "core/vfs/FileEntry.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

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

/// One thing a file says about itself, as a search sees it.
///
/// Narrower than the SDK's FileFact on purpose: a label and a display string
/// are what a panel shows, and a search needs only what can be asked about. The
/// plugin layer converts, which keeps this layer from depending on the
/// extension point that feeds it.
struct SearchFact
{
    QString key; ///< namespaced and stable: "image.camera", "media.duration"
    QString text; ///< what a text query is compared against
    double number = 0; ///< the same fact as a number, when it is one
    bool hasNumber = false;
};

/// What a criterion needs when the listing cannot answer it.
///
/// One reader rather than two, because the two questions differ only in how
/// much they ask for: what a file *is* comes from its first page, and what is
/// *in* it comes from that page and every one after.
struct SearchIo
{
    /// `bytes` from `offset`. Shorter than asked for at the end of the file,
    /// and empty when it could not be read -- which is not the same as a file
    /// that does not match.
    std::function<QByteArray(const VfsUri&, qint64 offset, qint64 bytes)> read;

    /// What the file says about itself, for a criterion an unscanned drive has
    /// no row to answer from. Supplied by whoever holds the reader registry;
    /// absent means the criterion cannot be answered here rather than that
    /// every file passes it.
    std::function<QList<SearchFact>(const FileEntry&)> facts;

    /// Whether the search has been called off. Polled between windows, because
    /// this is the one search that can take minutes and stopping it has to stop
    /// the reading rather than only the listing.
    std::function<bool()> cancelled;

    /// The most of one file that may be read.
    ///
    /// A 40 GB disk image is not searched by accident, and on a remote drive
    /// reading is downloading -- so the number is smaller there. A file over
    /// it is skipped and said to be skipped, never quietly passed over.
    qint64 ceiling = kLocalCeiling;

    /// What one file may cost. Sixteen megabytes locally: larger than anything
    /// anybody greps on purpose and small enough that a stray video costs a
    /// moment rather than a minute. A megabyte over a network, where the same
    /// read is a download somebody is paying for.
    static constexpr qint64 kLocalCeiling = 16 * 1024 * 1024;
    static constexpr qint64 kRemoteCeiling = 1024 * 1024;

    /// How much is read at a time, and how much of the previous window is kept
    /// in front of the next one. The overlap is what finds a match lying across
    /// a boundary; it bounds the longest match that can be found, which is why
    /// it is a page rather than a handful of bytes.
    static constexpr qint64 kWindowBytes = 256 * 1024;
    static constexpr qint64 kOverlapBytes = 4096;

    explicit operator bool() const { return static_cast<bool>(read); }
};

/// Why a file is a hit, when the search asked about what is inside it.
///
/// A content search that answers with a list of names makes somebody open every
/// one of them to find out which it meant.
struct ContentMatch
{
    QString line; ///< the matching line, trimmed
    int lineNumber = 0; ///< counting from one
    int column = -1; ///< where the match starts in `line`, or -1
    int length = 0;

    bool isValid() const { return lineNumber > 0; }
};

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
        /// What is inside it. The dearest criterion there is, and the reason
        /// the cost ladder exists.
        Content,
        /// Something the file says about itself: `image.camera`, `image.iso`,
        /// `media.duration`. A column on an indexed volume and a bounded read
        /// on one that was never scanned, which is why the cost is a member.
        Metadata,
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
    double numberValue = 0; ///< Metadata, where the fact is a number
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
    /// Whether a file the sniffer calls binary is searched as bytes. Never the
    /// default, and occasionally the only way to find something.
    bool includeBinary = false;

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
    /// Text inside the file. Literal unless `asRegex`, folded unless
    /// `caseSensitive`, and text files only unless `includeBinary` -- decided
    /// by what is in the file rather than by its suffix, because a suffix list
    /// is wrong about the files people actually have.
    static SearchPredicate content(const QString& text, bool asRegex = false, bool caseSensitive = false);
    /// A fact the file states about itself, matched as text.
    static SearchPredicate metadataIs(const QString& key, const QString& value);
    /// The same fact as a number, in a range. Either bound on its own.
    static SearchPredicate metadataAtLeast(const QString& key, double value);
    static SearchPredicate metadataAtMost(const QString& key, double value);
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
    /// Pure for everything a listing already answered. `io` supplies the file
    /// for the criteria that need it; a predicate that needs it and is given
    /// none **does not match**, because the alternative is quietly answering a
    /// question nobody asked.
    ///
    /// `whyOut`, when given, is filled in by a content match with the line it
    /// was found on.
    [[nodiscard]] bool matches(
        const FileEntry& entry, const SearchIo& io = {}, ContentMatch* whyOut = nullptr) const;
    /// True when answering this needs the file rather than the listing.
    [[nodiscard]] bool needsFile() const;
    /// True when it needs the whole file rather than a page of it, which is
    /// what makes a search worth warning somebody about before it starts.
    [[nodiscard]] bool readsWholeFile() const { return field == Field::Content; }

    // ---- written down ----------------------------------------------------
    //
    // A search that can be stored is a search that can outlive the tab that ran
    // it: saved, handed to a headless runner, put on a clock, or made one step
    // of a chain. Until this there was nothing to store -- the only thing that
    // persisted a search anywhere was a form's own twenty-seven fields, which
    // is the shape of a form rather than a query. See MOLE-163.
    //
    // Two rules, and both are about a stored query meaning next year what it
    // means today. **Enums are stored as names**, because Field has gained
    // values and will gain more, and a stored `3` quietly becomes a different
    // field the next time somebody inserts one. And **`cost` is not stored**:
    // it is the planner's own answer, decided afresh from where the search is
    // running, so a stored copy would be a second opinion that goes wrong the
    // first time a criterion is cheap on a volume where it used to be dear.

    [[nodiscard]] QJsonObject toJson() const;

    /// Nothing when the object names a field or a match this build does not
    /// know. Refused rather than defaulted: a stored query that will not load is
    /// something somebody can act on, and one that quietly means something else
    /// is not -- a criterion silently dropped from a chain that deletes files
    /// makes it delete more of them.
    ///
    /// A key that is absent takes the member's default; a key that is there and
    /// unreadable is a refusal. Unknown keys are ignored.
    [[nodiscard]] static std::optional<SearchPredicate> fromJson(const QJsonObject& json);

    /// What a criterion of this kind costs before anybody has looked at where it
    /// is running.
    ///
    /// The other half of not storing `cost`: a loaded query has to arrive with a
    /// cost, or a content search comes back looking like the cheapest criterion
    /// there is -- evaluated first, and reported by SearchPlan::needsFile() as
    /// something a source that cannot read files can answer. Derived from the
    /// field rather than remembered from the file, which is the point: the same
    /// criterion is a column on an indexed volume and a read on one that was
    /// never scanned, and the planner is what settles that.
    [[nodiscard]] static PredicateCost costOf(Field field);

    /// The stored names of the two enums, and back.
    [[nodiscard]] static QString fieldName(Field field);
    [[nodiscard]] static std::optional<Field> fieldFromName(const QString& name);
    [[nodiscard]] static QString matchName(Match match);
    [[nodiscard]] static std::optional<Match> matchFromName(const QString& name);
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
    /// The cap when nothing says otherwise, and what a limit of nought means.
    static constexpr int kDefaultLimit = 10000;
    int limit = kDefaultLimit;

    /// The cap to apply, whatever `limit` says.
    ///
    /// Nought or less means "unsaid", not "no results". A query built by hand,
    /// restored from JSON or written by a chain can carry it, and the two engines
    /// read it differently: the walk stopped on its first entry and reported
    /// "Stopped at 0 matches (limit reached)", while the index fell back to the
    /// default for the same query. One reading, in one place. See MOLE-372.
    [[nodiscard]] int effectiveLimit() const { return limit > 0 ? limit : kDefaultLimit; }

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

    /// The whole search, written down. See SearchPredicate::toJson().
    [[nodiscard]] QJsonObject toJson() const;

    /// Nothing when any criterion in it is one this build cannot read. One bad
    /// criterion refuses the whole query on purpose: a query that came back with
    /// three of its four criteria is a query that matches more files than
    /// whoever saved it asked for.
    [[nodiscard]] static std::optional<SearchQuery> fromJson(const QJsonObject& json);
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
    [[nodiscard]] bool needsFile() const;
    /// True when something left over reads whole files.
    [[nodiscard]] bool readsWholeFiles() const;

    /// Whether `entry` satisfies every criterion left over.
    ///
    /// Cheapest first, so the expensive ones are only reached by what survived
    /// everything else: filter to the PDFs, then open them.
    [[nodiscard]] bool matches(
        const FileEntry& entry, const SearchIo& io = {}, ContentMatch* whyOut = nullptr) const;

    /// The two halves of that, for a source that wants to do the second one
    /// several files at a time. Everything answerable from the listing, and
    /// then everything that needs the file.
    [[nodiscard]] bool matchesWithoutFile(const FileEntry& entry) const;
    [[nodiscard]] bool matchesNeedingFile(
        const FileEntry& entry, const SearchIo& io, ContentMatch* whyOut = nullptr) const;

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

/// The form two names are compared in when the question is whether they are the
/// **same node**, which is not the same question as whether one matches the
/// other.
///
/// Beside foldForSearch() so that the two can be seen to differ, because they
/// do: toLower() and toCaseFolded() disagree over a handful of code points --
/// Greek final sigma is the one to remember, "ΟΔΥΣΣΕΥΣ" lowercasing to a
/// different string than it folds to. VfsUri::equals() folds, so a duplicate
/// scan that lowercased put two names Mole calls one node in two buckets and
/// then compared neither with the other. Identity questions fold; matching
/// lowercases. See MOLE-341.
[[nodiscard]] QString foldForIdentity(const QString& text);

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

/// Looks for `predicate` inside the bytes `io` can read of `entry`.
///
/// Exposed for its own test: the window loop, the overlap that finds a match
/// across a boundary, the decoding and the ceiling are each a thing that can be
/// wrong on its own.
///
/// Returns an invalid match when there is none, when the file is binary and the
/// predicate did not ask for those, when it is over the ceiling, or when it
/// will not decode. `skippedOut`, when given, says which of those happened.
enum class ContentSkip {
    None,
    TooBig,
    Binary,
    Undecodable,
    Unreadable,
};
[[nodiscard]] ContentMatch findInContents(const FileEntry& entry, const SearchPredicate& predicate,
    const SearchIo& io, ContentSkip* skippedOut = nullptr);

} // namespace mole

Q_DECLARE_METATYPE(mole::ContentMatch)
Q_DECLARE_METATYPE(QList<mole::ContentMatch>)
