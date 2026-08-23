#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

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

private:
    QStringList m_order;
    QHash<QString, IChainStepKind*> m_kinds;
};

} // namespace mole
