#pragma once

#include "core/automation/Chain.h"
#include "core/search/SearchQuery.h"
#include "core/vfs/IFileSystem.h"

#include <functional>

namespace mole {

class FileSetStore;

/// How a step finds the drive a uri lives on.
///
/// A function rather than a VfsManager, so core's automation layer does not
/// depend on the mount table and a test can hand over one in-memory drive. In the
/// application it is `VfsManager::resolve`.
using DriveResolver = std::function<FileSystemPtr(const VfsUri&)>;

/// Where a chain begins when nothing handed it a list: a place, and how far down.
///
/// **A folder, a drive root or a file set, plus a depth.** `maxDepth` means what
/// it means everywhere else -- -1 is everything below, 0 is this folder and
/// nothing under it -- so the first step is a pair of fields rather than a form.
///
/// A file set is the odd one and is not a walk at all: its members are already a
/// list, they may sit on more than one drive, and depth means nothing to them.
class PlaceSource final : public IChainStepKind
{
public:
    /// The stored identity. Never renamed: saved chains name their steps by it.
    static QString stepKind() { return QStringLiteral("place"); }
    /// Where to look, as a uri.
    static QString whereKey() { return QStringLiteral("where"); }
    /// How far down, as maxDepth means it.
    static QString depthKey() { return QStringLiteral("depth"); }
    /// A file set by id, instead of a uri.
    static QString setKey() { return QStringLiteral("set"); }
    /// The criteria this place has absorbed from the filter that followed it.
    static QString filterKey() { return QStringLiteral("filter"); }

    /// `sets` may be null, and then a place naming a set says it cannot look
    /// there rather than quietly returning nothing.
    PlaceSource(DriveResolver resolver, const FileSetStore* sets = nullptr);

    QString kind() const override { return stepKind(); }
    QString displayName() const override { return QStringLiteral("Look in a place"); }
    StepRole role() const override { return StepRole::Source; }
    QList<StepParameter> parameters() const override;

    StepOutcome run(const ChainStep& step, const QStringList& incoming, const StepContext& context) override;
    /// Really walks, because a preview that does not name the files answers a
    /// different question. Reading a listing writes nothing.
    StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context) override;

    /// Takes a filter's criteria into the walk. See IChainStepKind::absorbs().
    bool absorbs(const IChainStepKind& next) const override;
    ChainStep absorb(const ChainStep& mine, const ChainStep& next) const override;

private:
    /// The walk, or the set's members, with whatever criteria were absorbed
    /// applied as the listing hands each entry over.
    /// `incomplete` is set when part of the tree could not be read, which is a
    /// different thing from finding nothing and is not allowed to look like it.
    /// See MOLE-353 and ADR-0030.
    QStringList collect(
        const ChainStep& step, const StepContext& context, QString* whyOut, bool* incomplete) const;

    DriveResolver m_resolve;
    const FileSetStore* m_sets = nullptr;
};

/// What was on screen when the chain was started.
///
/// Nearly free: the shell already asks the current tab for its `targetUris()`,
/// and a chain started from a pane is handed that list -- so this step is a way
/// to say *use it* rather than a new mechanism.
///
/// A scheduled chain is handed nothing, and then this produces nothing and says
/// why. That is the honest answer: a chain whose first step is "what I was
/// looking at" has no meaning at three in the morning.
class ListingSource final : public IChainStepKind
{
public:
    static QString stepKind() { return QStringLiteral("listing"); }

    QString kind() const override { return stepKind(); }
    QString displayName() const override { return QStringLiteral("What is selected"); }
    StepRole role() const override { return StepRole::Source; }
    QList<StepParameter> parameters() const override { return {}; }

    StepOutcome run(const ChainStep& step, const QStringList& incoming, const StepContext& context) override;
    StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context) override;
};

/// A query, run again every time.
///
/// Called QuerySource rather than SearchSource because that name is already the
/// enum saying *which engine a plan is for* -- and a step kind and an engine
/// choice sharing a name would be read wrongly by everybody exactly once.
///
/// **The step stores a SearchQuery and re-evaluates it**, which is the difference
/// between a chain that processes today's photographs and one that processes the
/// photographs that happened to be there when it was written. This is what
/// MOLE-163 was for: a query is a value that can be written down.
class QuerySource final : public IChainStepKind
{
public:
    static QString stepKind() { return QStringLiteral("search"); }
    /// Where to search, as a uri.
    static QString whereKey() { return QStringLiteral("where"); }
    /// The query itself, as SearchQuery::toJson() writes it.
    static QString queryKey() { return QStringLiteral("query"); }

    explicit QuerySource(DriveResolver resolver);

    QString kind() const override { return stepKind(); }
    QString displayName() const override { return QStringLiteral("Run a search"); }
    StepRole role() const override { return StepRole::Source; }
    QList<StepParameter> parameters() const override;

    StepOutcome run(const ChainStep& step, const QStringList& incoming, const StepContext& context) override;
    StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context) override;

private:
    /// `incomplete` as for PlaceSource::collect().
    QStringList search(
        const ChainStep& step, const StepContext& context, QString* whyOut, bool* incomplete) const;

    DriveResolver m_resolve;
};

/// The query a step stored, or nothing when it stored none.
///
/// Shared because three steps keep a query in their parameters the same way, and
/// a query that will not load must refuse rather than become an empty one --
/// which would match everything.
[[nodiscard]] std::optional<SearchQuery> queryFrom(const QVariantMap& parameters, const QString& key);
/// The other direction, for whoever builds a step.
void putQuery(QVariantMap& parameters, const QString& key, const SearchQuery& query);

/// The reader a criterion that needs the file itself is answered from.
///
/// Shared for the reason that made it worth finding: **a content criterion given no
/// reader does not match**, so a step that built no reader would answer *nothing
/// matches* for a search that has matches. A place with a filter fused into it built
/// one; a filter standing on its own did not, so the same criteria gave opposite
/// answers depending on which position the step sat in -- which is the one thing
/// MOLE-168 asserts they must not do.
///
/// `needed` is `SearchPlan::needsFile()`. Answering false hands back a reader that
/// reads nothing, which is what makes a filter over names and sizes cost no I/O at
/// all rather than an unused connection per entry.
[[nodiscard]] SearchIo readerFor(const FileSystemPtr& drive, const StepContext& context, bool needed);

} // namespace mole
