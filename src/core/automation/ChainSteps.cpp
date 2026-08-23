#include "core/automation/ChainSteps.h"

namespace mole {

FilterStep::FilterStep(DriveResolver resolver)
    : m_resolve(std::move(resolver))
{
}

QList<StepParameter> FilterStep::parameters() const
{
    StepParameter query;
    query.key = queryKey();
    query.label = QStringLiteral("Keep only files matching");
    query.kind = StepParameter::Kind::Text;
    query.required = true;
    return { query };
}

QStringList FilterStep::keep(
    const ChainStep& step, const QStringList& incoming, const StepContext& context, QString* whyOut) const
{
    const std::optional<SearchQuery> query = queryFrom(step.parameters, queryKey());
    if (!query) {
        // Refused rather than treated as "keep everything": a filter that
        // quietly stopped filtering hands the next step more files than anybody
        // asked it to act on.
        if (whyOut)
            *whyOut = QStringLiteral("This filter's criteria cannot be read by this version");
        return {};
    }
    if (query->predicates.isEmpty())
        return incoming;

    const SearchPlan plan = planSearch(*query, SearchSource::Walk);
    QStringList kept;
    for (const QString& uri : incoming) {
        if (context.cancel.isCancelled())
            return kept;
        const VfsUri target = VfsUri::fromString(uri);
        FileSystemPtr drive = m_resolve ? m_resolve(target) : nullptr;
        if (!drive)
            continue;
        // The entry has to be asked for: a uri carries a name and nothing else,
        // and a criterion about a size or a date needs the rest of it.
        const Result<FileEntry> entry = drive->stat(target);
        if (!entry.ok())
            continue;
        if (plan.matches(entry.value()))
            kept.append(uri);
    }
    return kept;
}

StepOutcome FilterStep::run(const ChainStep& step, const QStringList& incoming, const StepContext& context)
{
    QString why;
    const QStringList kept = keep(step, incoming, context, &why);
    if (kept.isEmpty())
        return StepOutcome::nothing(why);
    return StepOutcome::produced(kept);
}

StepPreview FilterStep::preview(
    const ChainStep& step, const QStringList& incoming, const StepContext& context)
{
    QString why;
    const QStringList kept = keep(step, incoming, context, &why);
    if (kept.isEmpty())
        return StepPreview::nothing(why.isEmpty() ? QStringLiteral("nothing matches") : why);
    return StepPreview::would(kept, QStringLiteral("keep %1 of %2").arg(kept.size()).arg(incoming.size()));
}

} // namespace mole
