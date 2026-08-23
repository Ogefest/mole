#include "core/automation/ChainSources.h"

#include "core/sets/FileSetStore.h"
#include "core/vfs/DirectoryWalker.h"

#include <QJsonObject>

namespace mole {

namespace {

    /// A place's own criteria, as a plan for a walk. Absent when it absorbed
    /// none, which is the ordinary case.
    std::optional<SearchPlan> planFor(const QVariantMap& parameters, const QString& key)
    {
        const std::optional<SearchQuery> query = queryFrom(parameters, key);
        if (!query)
            return std::nullopt;
        // Walk rather than Index: a place is a tree being read now, and nothing
        // has scanned it. What the walker cannot push down it applies itself, and
        // planSearch() is what says which is which.
        return planSearch(*query, SearchSource::Walk);
    }

    /// The reader a criterion that needs the file is answered from.
    ///
    /// The same one LiveSearchTask builds, for the same reason: a content
    /// criterion that is given no reader does not match, and a chain that
    /// silently answered *nothing matches* would be worse than one that took
    /// longer.
    SearchIo readerFor(const FileSystemPtr& drive, const StepContext& context, bool needed)
    {
        SearchIo io;
        if (!needed || !drive)
            return io;
        io.read = [drive](const VfsUri& uri, qint64 offset, qint64 bytes) -> QByteArray {
            Result<std::unique_ptr<QIODevice>> stream = drive->openRead(uri, offset + bytes);
            if (!stream.ok() || !stream.value())
                return {};
            if (offset > 0 && !stream.value()->seek(offset)) {
                if (stream.value()->read(offset).size() != offset)
                    return {};
            }
            return stream.value()->read(bytes);
        };
        io.cancelled = [cancel = context.cancel] { return cancel.isCancelled(); };
        return io;
    }

} // namespace

std::optional<SearchQuery> queryFrom(const QVariantMap& parameters, const QString& key)
{
    const QVariant held = parameters.value(key);
    if (!held.isValid())
        return std::nullopt;
    return SearchQuery::fromJson(QJsonObject::fromVariantMap(held.toMap()));
}

void putQuery(QVariantMap& parameters, const QString& key, const SearchQuery& query)
{
    parameters.insert(key, query.toJson().toVariantMap());
}

// ---- a place ---------------------------------------------------------------

PlaceSource::PlaceSource(DriveResolver resolver, const FileSetStore* sets)
    : m_resolve(std::move(resolver))
    , m_sets(sets)
{
}

QList<StepParameter> PlaceSource::parameters() const
{
    StepParameter where;
    where.key = whereKey();
    where.label = QStringLiteral("Where to look");
    where.kind = StepParameter::Kind::Uri;

    StepParameter depth;
    depth.key = depthKey();
    depth.label = QStringLiteral("How far down");
    depth.kind = StepParameter::Kind::Number;
    // Everything below, which is what somebody who says nothing means.
    depth.fallback = -1;

    StepParameter set;
    set.key = setKey();
    set.label = QStringLiteral("A file set, instead of a place");
    set.kind = StepParameter::Kind::Text;

    return { where, depth, set };
}

QStringList PlaceSource::collect(const ChainStep& step, const StepContext& context, QString* whyOut) const
{
    const QString setId = step.parameters.value(setKey()).toString();
    if (!setId.isEmpty()) {
        if (!m_sets) {
            if (whyOut)
                *whyOut = QStringLiteral("There are no file sets here to look in");
            return {};
        }
        // A set is already a list, and its members may sit on several drives --
        // so there is nothing to walk and no depth to honour.
        const FileSet set = m_sets->set(setId);
        if (set.id.isEmpty() && whyOut)
            *whyOut = QStringLiteral("There is no file set called %1 any more").arg(setId);
        return set.uris;
    }

    const VfsUri where = VfsUri::fromString(step.parameters.value(whereKey()).toString());
    if (!where.isValid()) {
        if (whyOut)
            *whyOut = QStringLiteral("This step has nowhere to look");
        return {};
    }
    FileSystemPtr drive = m_resolve ? m_resolve(where) : nullptr;
    if (!drive) {
        if (whyOut)
            *whyOut = QStringLiteral("Nothing is mounted for %1").arg(where.toString());
        return {};
    }

    DirectoryWalker::Options options;
    options.maxDepth = step.parameters.value(depthKey(), -1).toInt();
    DirectoryWalker walker(drive, options);

    const std::optional<SearchPlan> plan = planFor(step.parameters, filterKey());
    const SearchIo io = readerFor(drive, context, plan && plan->needsFile());

    QStringList found;
    const Result<void> walked = walker.walk(where, context.cancel, [&](const FileEntry& entry, int) {
        // The absorbed criteria are applied to the entry the listing already
        // handed over, which is the whole point of the fusion: a filter given
        // only uris would have to ask the drive about every one of them again.
        if (!plan || plan->matches(entry, io))
            found.append(entry.uri.toString());
        return DirectoryWalker::Action::Continue;
    });
    if (!walked.ok() && found.isEmpty() && whyOut)
        *whyOut = walked.error().message;
    return found;
}

StepOutcome PlaceSource::run(const ChainStep& step, const QStringList&, const StepContext& context)
{
    QString why;
    const QStringList found = collect(step, context, &why);
    if (found.isEmpty())
        return StepOutcome::nothing(why);
    return StepOutcome::produced(found);
}

StepPreview PlaceSource::preview(const ChainStep& step, const QStringList&, const StepContext& context)
{
    QString why;
    const QStringList found = collect(step, context, &why);
    const QString where = step.parameters.value(setKey()).toString().isEmpty()
        ? step.parameters.value(whereKey()).toString()
        : QStringLiteral("the file set %1").arg(step.parameters.value(setKey()).toString());
    if (found.isEmpty())
        return StepPreview::nothing(why.isEmpty() ? QStringLiteral("nothing in %1").arg(where) : why);
    return StepPreview::would(found, QStringLiteral("look in %1").arg(where));
}

bool PlaceSource::absorbs(const IChainStepKind& next) const
{
    // A filter, and only a filter: it is the one kind whose whole work is
    // criteria, and criteria are what a walk can apply as it goes.
    return next.kind() == QStringLiteral("filter");
}

ChainStep PlaceSource::absorb(const ChainStep& mine, const ChainStep& next) const
{
    ChainStep fused = mine;
    const std::optional<SearchQuery> theirs = queryFrom(next.parameters, QStringLiteral("query"));
    if (!theirs)
        return fused;

    std::optional<SearchQuery> ours = queryFrom(fused.parameters, filterKey());
    SearchQuery both = ours ? *ours : SearchQuery {};
    for (const SearchPredicate& predicate : theirs->predicates)
        both.add(predicate);
    // The filter's own excluded folders and depth are its business and it has
    // none: what a filter carries is criteria.
    putQuery(fused.parameters, filterKey(), both);
    return fused;
}

// ---- what was on screen ----------------------------------------------------

StepOutcome ListingSource::run(const ChainStep&, const QStringList&, const StepContext& context)
{
    if (context.startedWith.isEmpty()) {
        return StepOutcome::nothing(QStringLiteral("Nothing was selected when this chain started"));
    }
    return StepOutcome::produced(context.startedWith);
}

StepPreview ListingSource::preview(const ChainStep&, const QStringList&, const StepContext& context)
{
    if (context.startedWith.isEmpty()) {
        return StepPreview::nothing(QStringLiteral("nothing is selected, so this chain would stop here"));
    }
    return StepPreview::would(context.startedWith, QStringLiteral("start from what is selected"));
}

// ---- a search --------------------------------------------------------------

QuerySource::QuerySource(DriveResolver resolver)
    : m_resolve(std::move(resolver))
{
}

QList<StepParameter> QuerySource::parameters() const
{
    StepParameter where;
    where.key = whereKey();
    where.label = QStringLiteral("Where to search");
    where.kind = StepParameter::Kind::Uri;

    StepParameter query;
    query.key = queryKey();
    query.label = QStringLiteral("What to look for");
    query.kind = StepParameter::Kind::Text;
    query.required = true;

    return { where, query };
}

QStringList QuerySource::search(const ChainStep& step, const StepContext& context, QString* whyOut) const
{
    const std::optional<SearchQuery> query = queryFrom(step.parameters, queryKey());
    if (!query) {
        // A query this build cannot read is refused rather than run as an empty
        // one, which would match every file under the root. MOLE-163's rule, and
        // this is the place it protects.
        if (whyOut)
            *whyOut = QStringLiteral("This step's query cannot be read by this version");
        return {};
    }

    const VfsUri where = VfsUri::fromString(step.parameters.value(whereKey()).toString());
    if (!where.isValid()) {
        if (whyOut)
            *whyOut = QStringLiteral("This search has nowhere to look");
        return {};
    }
    FileSystemPtr drive = m_resolve ? m_resolve(where) : nullptr;
    if (!drive) {
        if (whyOut)
            *whyOut = QStringLiteral("Nothing is mounted for %1").arg(where.toString());
        return {};
    }

    // Re-evaluated on every run, which is the whole reason the step stores a
    // query and not a list: a chain saved a fortnight ago finds what is there
    // today.
    const SearchPlan plan = planSearch(*query, SearchSource::Walk);
    const SearchIo io = readerFor(drive, context, plan.needsFile());

    DirectoryWalker::Options options;
    options.maxDepth = query->maxDepth;
    DirectoryWalker walker(drive, options);

    QStringList found;
    walker.walk(where, context.cancel, [&](const FileEntry& entry, int) {
        if (query->isExcluded(entry.name) && entry.isDir)
            return DirectoryWalker::Action::SkipSubtree;
        if (plan.matches(entry, io))
            found.append(entry.uri.toString());
        return found.size() >= query->limit ? DirectoryWalker::Action::Stop
                                            : DirectoryWalker::Action::Continue;
    });
    return found;
}

StepOutcome QuerySource::run(const ChainStep& step, const QStringList&, const StepContext& context)
{
    QString why;
    const QStringList found = search(step, context, &why);
    if (found.isEmpty())
        return StepOutcome::nothing(why);
    return StepOutcome::produced(found);
}

StepPreview QuerySource::preview(const ChainStep& step, const QStringList&, const StepContext& context)
{
    QString why;
    const QStringList found = search(step, context, &why);
    if (found.isEmpty())
        return StepPreview::nothing(why.isEmpty() ? QStringLiteral("the search finds nothing") : why);
    return StepPreview::would(found, QStringLiteral("run the search again"));
}

} // namespace mole
