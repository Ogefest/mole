#pragma once

#include "core/automation/ChainSources.h"

namespace mole {

/// Narrowing a list, which is the commonest thing anybody does between two
/// operations.
///
/// **The criteria are SearchPredicates**, which is what makes this one vocabulary
/// rather than a second one nobody can keep in step -- and it is why a filter can
/// be folded into the place before it: the same predicates the walk can apply as
/// the listing hands each entry over. See PlaceSource::absorbs().
///
/// Usable at any position. First, where the fusion means it never runs as a step
/// at all; fourth, where it narrows whatever arrived. The answer is the same
/// either way, and a test holds it to that.
///
/// **A filter handed a list of uris has to ask the drive about each of them.** A
/// uri is not a FileEntry: a name can be read off it, but a size, a date and a
/// kind cannot. That is the cost the fusion exists to avoid, and it is why this
/// resolves a drive at all.
class FilterStep final : public IChainStepKind
{
public:
    static QString stepKind() { return QStringLiteral("filter"); }
    /// The criteria, as SearchQuery::toJson() writes them.
    static QString queryKey() { return QStringLiteral("query"); }

    explicit FilterStep(DriveResolver resolver);

    QString kind() const override { return stepKind(); }
    QString displayName() const override { return QStringLiteral("Keep only"); }
    StepRole role() const override { return StepRole::Transform; }
    QList<StepParameter> parameters() const override;

    /// **What this filter will cost, which is the whole of what there is to say
    /// about it.** A criterion about a name, an extension, a size or a date is
    /// answered from the entry the drive already hands over: free, exactly like the
    /// bar over a listing that this exists to be a step version of. A criterion that
    /// reaches into the contents opens every candidate, and on a remote drive
    /// opening is downloading -- which is the difference between a filter somebody
    /// leaves on a clock and one that pulls a volume across a link every night.
    ///
    /// Derived from `SearchPlan::needsFile()` rather than worked out here, so it is
    /// the same fact `LiveSearchController::readsFileContents` shows in the search
    /// view and cannot drift from it.
    bool readsFileContents(const ChainStep& step) const override;

    StepOutcome run(const ChainStep& step, const QStringList& incoming, const StepContext& context) override;
    /// Filtering writes nothing, so a preview is the real answer rather than an
    /// estimate of one.
    StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context) override;

private:
    QStringList keep(const ChainStep& step, const QStringList& incoming, const StepContext& context,
        QString* whyOut) const;

    DriveResolver m_resolve;
};

} // namespace mole
