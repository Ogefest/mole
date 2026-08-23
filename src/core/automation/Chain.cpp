#include "core/automation/Chain.h"

#include <QJsonArray>

namespace mole {

namespace {

    /// The stored names, in one table read both ways -- so a role added to the
    /// enum without a name saves as nothing and refuses to load, rather than
    /// half-working. Same arrangement as SearchPredicate's fields.
    struct RoleName
    {
        StepRole role;
        const char* name;
    };

    const QList<RoleName>& roleNames()
    {
        static const QList<RoleName> names {
            { StepRole::Source, "source" },
            { StepRole::Transform, "transform" },
            { StepRole::Sink, "sink" },
        };
        return names;
    }

    struct ParameterKindName
    {
        StepParameter::Kind kind;
        const char* name;
    };

    const QList<ParameterKindName>& parameterKindNames()
    {
        using Kind = StepParameter::Kind;
        static const QList<ParameterKindName> names {
            { Kind::Text, "text" },
            { Kind::Uri, "uri" },
            { Kind::Number, "number" },
            { Kind::Flag, "flag" },
            { Kind::Choice, "choice" },
        };
        return names;
    }

    /// How a step is named in a message about what is wrong with a chain.
    ///
    /// Its position and its kind, because a chain with two "compress" steps is
    /// ordinary and "the compress step is in the wrong place" would then name
    /// both of them.
    QString describe(const Chain& chain, int index, const IChainStepKind* kind)
    {
        const QString name = kind ? kind->displayName() : chain.steps.at(index).kind;
        return QStringLiteral("step %1 of %2 (%3)").arg(index + 1).arg(chain.steps.size()).arg(name);
    }

} // namespace

QString stepRoleToString(StepRole role)
{
    for (const RoleName& entry : roleNames()) {
        if (entry.role == role)
            return QString::fromLatin1(entry.name);
    }
    return {};
}

std::optional<StepRole> stepRoleFromString(const QString& name)
{
    for (const RoleName& entry : roleNames()) {
        if (QLatin1String(entry.name) == name)
            return entry.role;
    }
    return std::nullopt;
}

QString StepParameter::kindToString(Kind kind)
{
    for (const ParameterKindName& entry : parameterKindNames()) {
        if (entry.kind == kind)
            return QString::fromLatin1(entry.name);
    }
    return {};
}

std::optional<StepParameter::Kind> StepParameter::kindFromString(const QString& name)
{
    for (const ParameterKindName& entry : parameterKindNames()) {
        if (QLatin1String(entry.name) == name)
            return entry.kind;
    }
    return std::nullopt;
}

StepOutcome StepOutcome::produced(QStringList uris, QString message)
{
    StepOutcome outcome;
    // An empty list is Nothing whatever the caller called it: the distinction is
    // about what the next step would be handed, not about what a step meant to
    // say. A step that produced nothing and reported success is exactly the
    // mistake the third outcome exists to stop.
    outcome.result = uris.isEmpty() ? Result::Nothing : Result::Produced;
    outcome.uris = std::move(uris);
    outcome.message = std::move(message);
    return outcome;
}

StepOutcome StepOutcome::nothing(QString message)
{
    StepOutcome outcome;
    outcome.result = Result::Nothing;
    outcome.message = std::move(message);
    return outcome;
}

StepOutcome StepOutcome::failed(QString message)
{
    StepOutcome outcome;
    outcome.result = Result::Failed;
    outcome.message = std::move(message);
    return outcome;
}

StepPreview StepPreview::would(QStringList uris, QString intent)
{
    StepPreview preview;
    // The same normalisation StepOutcome::produced() makes: an empty list is
    // Nothing whatever it was called, because what matters is what the next step
    // would be handed.
    preview.result = uris.isEmpty() ? Result::Nothing : Result::Produced;
    preview.uris = std::move(uris);
    preview.intent = std::move(intent);
    return preview;
}

StepPreview StepPreview::nothing(QString intent)
{
    StepPreview preview;
    preview.result = Result::Nothing;
    preview.intent = std::move(intent);
    return preview;
}

StepPreview StepPreview::unpredictable(QString intent)
{
    StepPreview preview;
    preview.result = Result::Unpredictable;
    preview.intent = std::move(intent);
    return preview;
}

StepPreview StepPreview::cannotSay(QString intent)
{
    StepPreview preview;
    preview.result = Result::Failed;
    preview.intent = std::move(intent);
    return preview;
}

StepPreview IChainStepKind::preview(const ChainStep&, const QStringList&, const StepContext&)
{
    // The honest default: a kind that has not been taught to answer this must not
    // guess, and must not do the work to find out either.
    return StepPreview::unpredictable(QStringLiteral("cannot be predicted without running it"));
}

QList<StepParameter> IChainStepKind::chainProperties() const
{
    if (role() == StepRole::Sink)
        return {};

    StepParameter stop;
    stop.key = QString::fromLatin1(kStopWhenEmpty);
    stop.label = QStringLiteral("Stop the chain when this produces nothing");
    stop.kind = StepParameter::Kind::Flag;
    stop.fallback = true;
    return { stop };
}

bool IChainStepKind::absorbs(const IChainStepKind&) const
{
    return false;
}

ChainStep IChainStepKind::absorb(const ChainStep& mine, const ChainStep&) const
{
    return mine;
}

QJsonObject ChainStep::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("kind")] = kind;
    if (!parameters.isEmpty())
        json[QStringLiteral("parameters")] = QJsonObject::fromVariantMap(parameters);
    if (!properties.isEmpty())
        json[QStringLiteral("properties")] = QJsonObject::fromVariantMap(properties);
    return json;
}

std::optional<ChainStep> ChainStep::fromJson(const QJsonObject& json)
{
    ChainStep step;
    step.kind = json.value(QStringLiteral("kind")).toString();
    if (step.kind.isEmpty())
        return std::nullopt;
    step.parameters = json.value(QStringLiteral("parameters")).toObject().toVariantMap();
    step.properties = json.value(QStringLiteral("properties")).toObject().toVariantMap();
    return step;
}

bool ChainStep::stopsWhenEmpty() const
{
    const QVariant said = properties.value(QString::fromLatin1(kStopWhenEmpty));
    return said.isValid() ? said.toBool() : true;
}

QJsonObject Chain::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("name")] = name;
    json[QStringLiteral("enabled")] = enabled;
    QJsonArray stored;
    for (const ChainStep& step : steps)
        stored.append(step.toJson());
    json[QStringLiteral("steps")] = stored;
    return json;
}

std::optional<Chain> Chain::fromJson(const QJsonObject& json)
{
    Chain chain;
    chain.id = json.value(QStringLiteral("id")).toString();
    chain.name = json.value(QStringLiteral("name")).toString();
    chain.enabled = json.value(QStringLiteral("enabled")).toBool(true);

    const QJsonArray stored = json.value(QStringLiteral("steps")).toArray();
    for (const QJsonValue& value : stored) {
        const std::optional<ChainStep> step = ChainStep::fromJson(value.toObject());
        if (!step)
            return std::nullopt;
        chain.steps.append(*step);
    }
    return chain;
}

void ChainRegistry::registerKind(IChainStepKind* kind)
{
    if (!kind || kind->kind().isEmpty())
        return;
    const QString id = kind->kind();
    if (!m_kinds.contains(id))
        m_order.append(id);
    m_kinds.insert(id, kind);
}

IChainStepKind* ChainRegistry::kind(const QString& id) const
{
    return m_kinds.value(id, nullptr);
}

QStringList ChainRegistry::kinds() const
{
    return m_order;
}

Chain ChainRegistry::fused(const Chain& chain) const
{
    Chain out = chain;
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i + 1 < out.steps.size(); ++i) {
            IChainStepKind* mine = kind(out.steps.at(i).kind);
            IChainStepKind* next = kind(out.steps.at(i + 1).kind);
            if (!mine || !next || !mine->absorbs(*next))
                continue;
            out.steps[i] = mine->absorb(out.steps.at(i), out.steps.at(i + 1));
            out.steps.removeAt(i + 1);
            // Start again rather than carry on from here: absorbing one filter
            // can leave the next one absorbable too.
            again = true;
            break;
        }
    }
    return out;
}

bool ChainRegistry::isRunnable(const Chain& chain, QString* whyOut) const
{
    const auto refuse = [whyOut](const QString& why) {
        if (whyOut)
            *whyOut = why;
        return false;
    };

    if (chain.steps.isEmpty())
        return refuse(QStringLiteral("This chain has no steps, so there is nothing for it to do"));

    for (int i = 0; i < chain.steps.size(); ++i) {
        const ChainStep& step = chain.steps.at(i);
        IChainStepKind* kind = this->kind(step.kind);
        if (!kind) {
            return refuse(QStringLiteral("%1 is a \"%2\", which nothing here knows how to run")
                              .arg(describe(chain, i, nullptr), step.kind));
        }

        const bool last = i == chain.steps.size() - 1;
        switch (kind->role()) {
        case StepRole::Sink:
            if (!last) {
                return refuse(QStringLiteral("%1 ends a chain: it takes a list and gives nothing back, so "
                                             "nothing after it would have anything to work on")
                                  .arg(describe(chain, i, kind)));
            }
            break;
        case StepRole::Source:
            if (i != 0) {
                return refuse(QStringLiteral("%1 takes nothing, so putting it here would throw away "
                                             "everything the steps before it found")
                                  .arg(describe(chain, i, kind)));
            }
            break;
        case StepRole::Transform:
            if (i == 0) {
                return refuse(QStringLiteral("%1 needs a list of files, and there is nothing "
                                             "before it to produce one")
                                  .arg(describe(chain, i, kind)));
            }
            break;
        }
    }

    if (whyOut)
        whyOut->clear();
    return true;
}

} // namespace mole
