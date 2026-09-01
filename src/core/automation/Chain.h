#pragma once

#include "core/vfs/VfsTypes.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace mole {

/// What a step takes and what it gives.
///
/// Three roles and no more, and the table is the whole of it:
///
/// | role      | takes    | gives    |
/// |-----------|----------|----------|
/// | source    | nothing  | a list   |
/// | transform | a list   | a list   |
/// | sink      | a list   | nothing  |
///
/// A sink can only be last, and that is a property of the role rather than a
/// setting somebody can get wrong -- which is what makes "a report on a clock"
/// an ordinary one-step chain instead of a special case.
enum class StepRole {
    Source,
    Transform,
    Sink,
};

QString stepRoleToString(StepRole role);
/// Nothing for a name this build does not know, rather than the first
/// enumerator: a chain whose step claims a role nobody recognises must refuse to
/// load rather than quietly become a source. Same rule as SearchPredicate's
/// stored enums -- see MOLE-163.
std::optional<StepRole> stepRoleFromString(const QString& name);

/// One value a step needs before it can run, as the step kind describes it.
///
/// A declaration rather than a form: the kind says what it needs and what a
/// reasonable answer looks like, and whoever builds a chain -- a person at a
/// screen, a test, a saved file -- fills it in. Nothing here knows about QML.
struct StepParameter
{
    enum class Kind {
        Text,
        Uri, ///< a place; offered as somewhere to browse to
        Number,
        Flag,
        Choice, ///< one of `choices`
    };

    QString key;
    QString label;
    Kind kind = Kind::Text;
    QStringList choices;
    /// What the step means when it says nothing. A parameter with a fallback is
    /// one somebody can leave alone.
    QVariant fallback;
    /// A step missing this cannot run at all, which is different from one that
    /// left a choice at its default.
    bool required = false;

    static QString kindToString(Kind kind);
    static std::optional<Kind> kindFromString(const QString& name);
};

/// The key of the one chain-only property there is so far.
///
/// **Chain-only properties are not inputs to the operation.** They are decisions
/// about how the line behaves around it, and this is the first: whether a step
/// that produced nothing stops the chain or hands on an empty list. Stopping is
/// the default, because a chain whose search found nothing and carried on is a
/// chain that then acted on whatever it was given by accident.
///
/// They are declared by the step kind and set on the step. The chain never
/// reaches in and configures behaviour from outside -- the same rule a backend
/// follows about its own fields.
inline constexpr auto kStopWhenEmpty = "stopWhenEmpty";

/// What running one step gave back.
///
/// Three outcomes and not two, and the third is the point: **a step that produced
/// nothing is not a step that failed, and it is not a step that succeeded
/// either.** A chain that carried on with nothing writes an empty archive and
/// moves it somewhere, which reads exactly like working. The scheduler already
/// draws this distinction between Skipped and Failed, for the same reason.
struct StepOutcome
{
    enum class Result {
        Produced, ///< here is the list
        Nothing, ///< it ran, it worked, and there was nothing
        Failed, ///< it did not do what it said it would
    };

    Result result = Result::Produced;
    /// What to hand to the next step. Empty for Nothing and for Failed.
    QStringList uris;
    /// What happened, in words somebody reading a task strip can use. Required
    /// for a failure, welcome otherwise.
    QString message;

    static StepOutcome produced(QStringList uris, QString message = {});
    static StepOutcome nothing(QString message = {});
    static StepOutcome failed(QString message);

    [[nodiscard]] bool ok() const { return result != Result::Failed; }
};

/// What a step says it *would* do, asked without doing any of it.
///
/// **A preview that does not name the actual files answers a different question
/// from the one asked**, so sources and filters genuinely evaluate: a dry run
/// lists the forty-one files it found, not the fact that it found forty-one.
/// Everything that writes reports its intent instead -- *compress 41 files into
/// reports.zip*, *move 1 file to the archive drive*.
///
/// The third result is the honest one. A step whose output cannot be known without
/// doing the work says so, and the preview ends there saying how many steps are
/// left -- rather than inventing a list, or stopping in a way that makes the chain
/// look shorter than it is. Compression is the interesting case in the other
/// direction: it produces one archive whose name is already known, so the step
/// after it can still be planned.
struct StepPreview
{
    enum class Result {
        Produced, ///< here is the list it would hand on
        Nothing, ///< it would produce nothing, which stops the chain as usual
        Unpredictable, ///< it cannot know without doing the work
        Failed, ///< it could not even work out what it would do
    };

    Result result = Result::Unpredictable;
    /// What would be handed to the next step. Named, not counted.
    QStringList uris;
    /// What this step would do, in words: the line a person reads before saying
    /// yes. Required for anything that writes, whose list says nothing about what
    /// it would write.
    QString intent;

    static StepPreview would(QStringList uris, QString intent = {});
    static StepPreview nothing(QString intent = {});
    static StepPreview unpredictable(QString intent = {});
    static StepPreview cannotSay(QString intent);
};

/// What a step is given while it runs, beside its own parameters.
///
/// Not a Task, deliberately: a step runs *inside* the chain's task, on the pool
/// thread the chain is already on. Submitting a task per step would take the
/// chain's cancellation away from it -- the chain would be waiting on something
/// it cannot stop -- and would put a row in the strip per step for work somebody
/// asked for once.
struct StepContext
{
    /// The chain's own token. A step that runs for any length of time polls it,
    /// because cancelling a chain has to reach the step that is running rather
    /// than only the gap after it.
    CancelToken cancel;
    /// A line for the strip: what this step is doing at the moment.
    std::function<void(const QString& text)> say;
    /// How far through this step is, when it can say. Feeds the chain's own
    /// progress under the step count.
    std::function<void(qint64 done, qint64 total)> progress;
    /// What the chain was started from -- a pane's selection, usually. Empty for
    /// a chain nothing handed a list to, which is what a scheduled run looks
    /// like, and a source that needs one then produces nothing and says so
    /// rather than acting on whatever it can find.
    QStringList startedWith;
};

/// One step of a chain: which kind, and what it was given.
struct ChainStep
{
    /// Names a registered kind. A step whose kind is not registered is a step
    /// nothing can run, and a chain holding one refuses to run rather than
    /// skipping it.
    QString kind;
    /// What the operation was given, by the kind's own parameter keys.
    QVariantMap parameters;
    /// What the chain was told about this step -- see kStopWhenEmpty. Separate
    /// from `parameters` because they are answers to different questions, and a
    /// step kind's parameters are its own business.
    QVariantMap properties;

    [[nodiscard]] QJsonObject toJson() const;
    /// Nothing for a step with no kind: a step nobody can name is not a step,
    /// and dropping it silently would change what the chain does.
    [[nodiscard]] static std::optional<ChainStep> fromJson(const QJsonObject& json);

    /// Whether a step that produced nothing stops the line. True unless the step
    /// says otherwise.
    [[nodiscard]] bool stopsWhenEmpty() const;
};

/// One kind of step: what it is, what it does, and what it needs.
///
/// Implemented by whoever owns the work, the way IScheduledJob already is -- so
/// compression can be a step without core learning what an archive is, and a
/// plugin's step is offered exactly like a built-in one.
///
/// **Running is not here.** A vocabulary that can be listed, saved and validated
/// is worth having before anything can run a chain, and mixing the two would mean
/// a step kind nobody can describe without also being able to execute it.
class IChainStepKind
{
public:
    virtual ~IChainStepKind() = default;

    /// The stored identity, stable for ever: "search", "compress", "move". A
    /// saved chain names its steps by this, so it is the one thing that may
    /// never be renamed.
    virtual QString kind() const = 0;
    /// What a person sees in a list of steps.
    virtual QString displayName() const = 0;
    virtual StepRole role() const = 0;
    /// What the operation needs.
    virtual QList<StepParameter> parameters() const = 0;

    /// What the *chain* needs, which is a different list. The default is the one
    /// property every step that produces something has; a sink produces nothing,
    /// so there is nothing after it to stop.
    virtual QList<StepParameter> chainProperties() const;

    /// Whether this step, configured like this, will open the files it is given.
    ///
    /// The one cost worth stating before a chain is left running: reading every
    /// candidate is a different proposition from reading a listing, and on a remote
    /// drive reading is downloading. Asked of the step rather than computed by
    /// whoever displays it, because only the step knows what its own parameters
    /// mean -- a filter derives it from `SearchPlan::needsFile()`.
    ///
    /// False by default, which is right for every step that acts on a list without
    /// looking inside what is on it. A kind that reads files unconditionally says so
    /// by returning true; the parameter is there for the ones where it depends.
    virtual bool readsFileContents(const ChainStep& step) const;

    /// Does the work, on the chain's own thread, and says what came of it.
    ///
    /// `incoming` is what the step before produced -- empty for a source, which
    /// takes nothing. A sink returns Nothing when it worked, because there is no
    /// list after it; that is not an empty result and nothing follows it to stop.
    ///
    /// **Runs to completion or to cancellation, and never throws.** A kind whose
    /// work is a Task of its own runs that work here rather than submitting it:
    /// see StepContext.
    virtual StepOutcome run(const ChainStep& step, const QStringList& incoming, const StepContext& context)
        = 0;

    /// What it would produce, and what it would do, without doing any of it.
    ///
    /// **Must write nothing.** A step that cannot answer without writing does not
    /// answer: the default here is Unpredictable, which is the honest reply for a
    /// kind that has not thought about it, and it costs the preview the steps
    /// after this one rather than costing somebody their files.
    ///
    /// A source or a filter overrides this and really evaluates, because a preview
    /// naming no files is a preview of nothing.
    virtual StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context);

    // ---- taking the next step's work into this one ------------------------
    //
    // **The fusion planSearch() already makes, one level up.** Walking a large
    // tree and discarding nine tenths of the result throws away the pushdown the
    // search planner exists to provide -- and a filter handed a list of uris has
    // to ask the drive about every one of them again, because a uri is not a
    // FileEntry. Fused into the place that produced them, the same criteria are
    // answered from the listing the walk already had.
    //
    // The line is what somebody built; the plan is what runs; nothing is dropped
    // in between. Whether it happened is said in the preview, so a dry run does
    // not hide it.

    /// Whether this kind can do `next`'s work as part of its own.
    virtual bool absorbs(const IChainStepKind& next) const;
    /// This step, with `next`'s work folded into it. Only called when absorbs()
    /// said yes.
    virtual ChainStep absorb(const ChainStep& mine, const ChainStep& next) const;
};

/// Everything a chain is.
///
/// **A chain is a line, not a graph, and this is where that is refused rather
/// than only planned.** No branches, no joins, no conditionals, no variables, no
/// expressions -- because each of those arrives on its own, looking small, and
/// what they add up to is a scripting language with no debugger. The precedent is
/// parseQueryLine(), which refuses boolean operators in its own header for the
/// same reason. Somebody who needs a graph needs a different tool, and saying so
/// is kinder than half of one.
///
/// The currency between steps is a list of uris and nothing else. See
/// docs/adr/0082-a-chain-is-a-line-and-a-list-of-uris-passes-along-it.md.
struct Chain
{
    QString id;
    QString name;
    QList<ChainStep> steps;
    /// Chains are offered in the order somebody made them, so the list does not
    /// reorder itself as they are renamed.
    bool enabled = true;

    [[nodiscard]] QJsonObject toJson() const;
    /// Nothing when a step in it cannot be read -- one unreadable step refuses
    /// the whole chain, for the reason a query with a missing criterion is
    /// refused: what is left is not what anybody saved.
    [[nodiscard]] static std::optional<Chain> fromJson(const QJsonObject& json);
};

/// Which step kinds exist.
///
/// Contributable, and for a plain reason: CompressTask lives in the archive
/// plugin, so a registry that only knew about core could not offer compression at
/// all. Replacing a kind is allowed, the way Scheduler::registerJob() allows it,
/// so a plugin reloading does not orphan the chains that use it.
///
/// Does not own what it is given -- whoever registers a kind outlives the
/// registration, as with the scheduler's jobs.
class ChainRegistry
{
public:
    void registerKind(IChainStepKind* kind);
    [[nodiscard]] IChainStepKind* kind(const QString& id) const;
    /// In the order they were registered, so a list on a screen does not shuffle
    /// between runs.
    [[nodiscard]] QStringList kinds() const;

    /// Whether `chain` could be run as it stands, and why not when it could not.
    ///
    /// Four ways a line can be wrong, and each message names the step:
    ///
    /// - it has no steps at all;
    /// - a step names a kind nothing here knows;
    /// - a **sink is not last** -- the rule the roles exist to state;
    /// - a **source is not first**, which is the same table read the other way:
    ///   a source takes nothing, so one in the middle throws away everything the
    ///   steps before it produced, and a chain that starts with a transform has
    ///   nothing to hand it.
    [[nodiscard]] bool isRunnable(const Chain& chain, QString* whyOut = nullptr) const;

    /// The same chain with every fusion made: what actually runs.
    ///
    /// Left to right and repeatedly, so a place followed by two filters absorbs
    /// both. A chain whose kinds absorb nothing comes back unchanged, which is
    /// the ordinary case.
    [[nodiscard]] Chain fused(const Chain& chain) const;

private:
    QStringList m_order;
    QHash<QString, IChainStepKind*> m_kinds;
};

} // namespace mole
