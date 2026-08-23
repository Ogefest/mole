#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/Chain.h"
#include "core/automation/ChainStore.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

/// A step kind of a given role, which is all most of these cases need.
class Kind final : public IChainStepKind
{
public:
    Kind(QString id, StepRole role, QList<StepParameter> parameters = {})
        : m_id(std::move(id))
        , m_role(role)
        , m_parameters(std::move(parameters))
    {
    }

    QString kind() const override { return m_id; }
    QString displayName() const override { return m_id + QStringLiteral(" step"); }
    StepRole role() const override { return m_role; }
    QList<StepParameter> parameters() const override { return m_parameters; }

private:
    QString m_id;
    StepRole m_role;
    QList<StepParameter> m_parameters;
};

ChainStep stepOf(const QString& kind)
{
    ChainStep step;
    step.kind = kind;
    return step;
}

} // namespace

/// What an operation is, said well enough to put it in a line.
///
/// Mole has a dozen operations and had no way to say what any of them takes or
/// produces, so nothing could ask the one question a chain needs answered. See
/// MOLE-164 and ADR-0082.
class TestChain : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void aKindCanBeRegisteredListedAndAsked();
    void registeringAKindAgainReplacesItRatherThanDuplicatingIt();
    void aChainRoundTripsWithItsStepsTheirParametersAndTheirOrder();
    void aStepWithNoKindRefusesTheWholeChain();
    void aSinkAnywhereButLastIsRefusedAndTheMessageNamesTheStep();
    void aSourceAnywhereButFirstIsRefusedForTheOppositeReason();
    void aChainThatStartsWithATransformHasNothingToHandIt();
    void aStepNothingKnowsHowToRunIsNamedRatherThanSkipped();
    void anEmptyChainIsNotRunnable();
    void anEmptyResultStopsTheChainUnlessTheStepSaysOtherwise();
    void aSinkDeclaresNoChainPropertiesBecauseNothingFollowsIt();
    void everyRoleAndEveryParameterKindHasAStoredName();
    void chainsSurviveBeingSavedAndLoaded();
    void aChainThatWillNotLoadIsDroppedAndCounted();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    ChainRegistry m_registry;
    std::vector<std::unique_ptr<Kind>> m_kinds;

    /// Registers a kind and keeps it alive: the registry does not own what it is
    /// given, the same way the scheduler does not own its jobs.
    void give(const QString& id, StepRole role, QList<StepParameter> parameters = {})
    {
        m_kinds.push_back(std::make_unique<Kind>(id, role, std::move(parameters)));
        m_registry.registerKind(m_kinds.back().get());
    }
};

void TestChain::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_registry = ChainRegistry {};
    m_kinds.clear();
}

void TestChain::aKindCanBeRegisteredListedAndAsked()
{
    StepParameter format;
    format.key = QStringLiteral("format");
    format.label = QStringLiteral("Format");
    format.kind = StepParameter::Kind::Choice;
    format.choices = { QStringLiteral("zip"), QStringLiteral("tar.gz") };
    format.fallback = QStringLiteral("zip");
    give(QStringLiteral("compress"), StepRole::Transform, { format });

    QCOMPARE(m_registry.kinds(), QStringList { QStringLiteral("compress") });
    IChainStepKind* kind = m_registry.kind(QStringLiteral("compress"));
    QVERIFY(kind != nullptr);
    QVERIFY(kind->role() == StepRole::Transform);
    QCOMPARE(kind->parameters().size(), 1);
    QCOMPARE(kind->parameters().first().key, QStringLiteral("format"));
    QCOMPARE(kind->parameters().first().choices.size(), 2);
    QCOMPARE(kind->parameters().first().fallback.toString(), QStringLiteral("zip"));

    // Nothing pretends a kind exists.
    QVERIFY(m_registry.kind(QStringLiteral("teleport")) == nullptr);
}

void TestChain::registeringAKindAgainReplacesItRatherThanDuplicatingIt()
{
    // A plugin reloading must not orphan the chains that name its kind, which is
    // why Scheduler::registerJob() allows this and so does this.
    give(QStringLiteral("compress"), StepRole::Transform);
    give(QStringLiteral("compress"), StepRole::Sink);

    QCOMPARE(m_registry.kinds().size(), 1);
    QVERIFY(m_registry.kind(QStringLiteral("compress"))->role() == StepRole::Sink);
}

void TestChain::aChainRoundTripsWithItsStepsTheirParametersAndTheirOrder()
{
    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.name = QStringLiteral("Tidy the reports");

    ChainStep search = stepOf(QStringLiteral("search"));
    search.parameters
        = { { QStringLiteral("where"), QStringLiteral("mem:///reports") }, { QStringLiteral("depth"), 3 } };
    search.properties = { { QString::fromLatin1(kStopWhenEmpty), false } };

    ChainStep compress = stepOf(QStringLiteral("compress"));
    compress.parameters = { { QStringLiteral("format"), QStringLiteral("zip") } };

    ChainStep move = stepOf(QStringLiteral("move"));
    move.parameters = { { QStringLiteral("to"), QStringLiteral("mem:///archive") } };

    chain.steps = { search, compress, move };

    const std::optional<Chain> back = Chain::fromJson(chain.toJson());
    QVERIFY(back.has_value());
    QCOMPARE(back->id, chain.id);
    QCOMPARE(back->name, chain.name);
    QCOMPARE(back->enabled, chain.enabled);
    QCOMPARE(back->steps.size(), 3);

    // The order is the chain: two steps swapped is a different chain that would
    // still load, which is why this is asserted rather than assumed.
    QCOMPARE(back->steps.at(0).kind, QStringLiteral("search"));
    QCOMPARE(back->steps.at(1).kind, QStringLiteral("compress"));
    QCOMPARE(back->steps.at(2).kind, QStringLiteral("move"));

    QCOMPARE(back->steps.at(0).parameters.value(QStringLiteral("where")).toString(),
        QStringLiteral("mem:///reports"));
    QCOMPARE(back->steps.at(0).parameters.value(QStringLiteral("depth")).toInt(), 3);
    QCOMPARE(back->steps.at(2).parameters.value(QStringLiteral("to")).toString(),
        QStringLiteral("mem:///archive"));
    // And a chain-only property, which is not a parameter and does not become one.
    QVERIFY2(!back->steps.at(0).stopsWhenEmpty(), "a step told to carry on stopped");
    QVERIFY2(back->steps.at(0).parameters.value(QString::fromLatin1(kStopWhenEmpty)).isNull(),
        "a chain property leaked into the operation's own parameters");
}

void TestChain::aStepWithNoKindRefusesTheWholeChain()
{
    // The MOLE-163 rule, applied here: a step nobody can name is not a step, and
    // a chain that loaded without it would do less than what somebody saved.
    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("search")), stepOf(QStringLiteral("move")) };
    QJsonObject object = chain.toJson();
    QJsonArray steps = object.value(QStringLiteral("steps")).toArray();
    QJsonObject broken = steps.at(1).toObject();
    broken.remove(QStringLiteral("kind"));
    steps.replace(1, broken);
    object[QStringLiteral("steps")] = steps;

    QVERIFY2(!Chain::fromJson(object).has_value(), "a chain came back with a step missing");
}

void TestChain::aSinkAnywhereButLastIsRefusedAndTheMessageNamesTheStep()
{
    give(QStringLiteral("search"), StepRole::Source);
    give(QStringLiteral("move"), StepRole::Sink);
    give(QStringLiteral("compress"), StepRole::Transform);

    Chain chain;
    chain.steps = { stepOf(QStringLiteral("search")), stepOf(QStringLiteral("move")),
        stepOf(QStringLiteral("compress")) };

    QString why;
    QVERIFY2(!m_registry.isRunnable(chain, &why), "a sink in the middle was accepted");
    // Its position and its name, because a chain with two of the same kind is
    // ordinary and "the move step" would then name both.
    QVERIFY2(why.contains(QStringLiteral("step 2 of 3")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("move step")), qPrintable(why));

    // The same chain with the sink last is fine, which is what says the refusal
    // is about the position rather than about the step.
    chain.steps = { stepOf(QStringLiteral("search")), stepOf(QStringLiteral("compress")),
        stepOf(QStringLiteral("move")) };
    QVERIFY2(m_registry.isRunnable(chain, &why), qPrintable(why));
    QVERIFY(why.isEmpty());
}

void TestChain::aSourceAnywhereButFirstIsRefusedForTheOppositeReason()
{
    give(QStringLiteral("search"), StepRole::Source);
    give(QStringLiteral("compress"), StepRole::Transform);

    Chain chain;
    chain.steps = { stepOf(QStringLiteral("search")), stepOf(QStringLiteral("compress")),
        stepOf(QStringLiteral("search")) };

    QString why;
    QVERIFY2(!m_registry.isRunnable(chain, &why), "a source in the middle was accepted");
    QVERIFY2(why.contains(QStringLiteral("step 3 of 3")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("throw away")), qPrintable(why));
}

void TestChain::aChainThatStartsWithATransformHasNothingToHandIt()
{
    give(QStringLiteral("compress"), StepRole::Transform);

    Chain chain;
    chain.steps = { stepOf(QStringLiteral("compress")) };

    QString why;
    QVERIFY(!m_registry.isRunnable(chain, &why));
    QVERIFY2(why.contains(QStringLiteral("step 1 of 1")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("needs a list")), qPrintable(why));
}

void TestChain::aStepNothingKnowsHowToRunIsNamedRatherThanSkipped()
{
    give(QStringLiteral("search"), StepRole::Source);

    Chain chain;
    chain.steps = { stepOf(QStringLiteral("search")), stepOf(QStringLiteral("transcode")) };

    QString why;
    QVERIFY(!m_registry.isRunnable(chain, &why));
    QVERIFY2(why.contains(QStringLiteral("transcode")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("step 2 of 2")), qPrintable(why));
}

void TestChain::anEmptyChainIsNotRunnable()
{
    QString why;
    QVERIFY(!m_registry.isRunnable(Chain {}, &why));
    QVERIFY2(why.contains(QStringLiteral("no steps")), qPrintable(why));
}

void TestChain::anEmptyResultStopsTheChainUnlessTheStepSaysOtherwise()
{
    // Stopping is the default, because a chain whose search found nothing and
    // carried on is a chain that acted on whatever it was given by accident.
    ChainStep step = stepOf(QStringLiteral("search"));
    QVERIFY(step.stopsWhenEmpty());

    step.properties = { { QString::fromLatin1(kStopWhenEmpty), false } };
    QVERIFY(!step.stopsWhenEmpty());

    // And it survives being written down, which is the whole point of it being a
    // property of the step rather than something the chain decides.
    const std::optional<ChainStep> back = ChainStep::fromJson(step.toJson());
    QVERIFY(back.has_value());
    QVERIFY(!back->stopsWhenEmpty());
}

void TestChain::aSinkDeclaresNoChainPropertiesBecauseNothingFollowsIt()
{
    give(QStringLiteral("search"), StepRole::Source);
    give(QStringLiteral("move"), StepRole::Sink);

    const QList<StepParameter> ofSource = m_registry.kind(QStringLiteral("search"))->chainProperties();
    QCOMPARE(ofSource.size(), 1);
    QCOMPARE(ofSource.first().key, QString::fromLatin1(kStopWhenEmpty));
    QVERIFY(ofSource.first().kind == StepParameter::Kind::Flag);
    QVERIFY(ofSource.first().fallback.toBool());

    // Nothing follows a sink, so there is nothing for an empty result to stop.
    QVERIFY(m_registry.kind(QStringLiteral("move"))->chainProperties().isEmpty());
}

void TestChain::everyRoleAndEveryParameterKindHasAStoredName()
{
    // The same pin as the search predicate's enums: a value added without a name
    // fails here rather than the first time a saved chain will not load.
    for (StepRole role : { StepRole::Source, StepRole::Transform, StepRole::Sink }) {
        const QString name = stepRoleToString(role);
        QVERIFY(!name.isEmpty());
        const std::optional<StepRole> back = stepRoleFromString(name);
        QVERIFY(back.has_value());
        QVERIFY(*back == role);
    }
    QVERIFY(!stepRoleFromString(QStringLiteral("fanOut")).has_value());

    using Kind = StepParameter::Kind;
    for (Kind kind : { Kind::Text, Kind::Uri, Kind::Number, Kind::Flag, Kind::Choice }) {
        const QString name = StepParameter::kindToString(kind);
        QVERIFY(!name.isEmpty());
        const std::optional<Kind> back = StepParameter::kindFromString(name);
        QVERIFY(back.has_value());
        QVERIFY(*back == kind);
    }
    QVERIFY(!StepParameter::kindFromString(QStringLiteral("expression")).has_value());
}

void TestChain::chainsSurviveBeingSavedAndLoaded()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("chains.json"));
    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.name = QStringLiteral("Tidy the reports");
    ChainStep search = stepOf(QStringLiteral("search"));
    search.parameters = { { QStringLiteral("where"), QStringLiteral("mem:///reports") } };
    chain.steps = { search, stepOf(QStringLiteral("compress")) };

    {
        ChainStore store(path);
        QSignalSpy changed(&store, &ChainStore::chainsChanged);
        QVERIFY(store.put(chain));
        QCOMPARE(changed.count(), 1);
        QVERIFY(store.save());

        // Editing in place rather than moving it to the end of the list.
        Chain renamed = chain;
        renamed.name = QStringLiteral("Tidy them properly");
        Chain second;
        second.id = QStringLiteral("c2");
        second.name = QStringLiteral("Another");
        QVERIFY(store.put(second));
        QVERIFY(store.put(renamed));
        QCOMPARE(store.chains().size(), 2);
        QCOMPARE(store.chains().first().name, QStringLiteral("Tidy them properly"));
        QVERIFY(store.save());
    }

    ChainStore reopened(path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.unreadable(), 0);
    QCOMPARE(reopened.chains().size(), 2);
    QCOMPARE(reopened.chain(QStringLiteral("c1")).steps.size(), 2);
    QCOMPARE(reopened.chain(QStringLiteral("c1"))
                 .steps.first()
                 .parameters.value(QStringLiteral("where"))
                 .toString(),
        QStringLiteral("mem:///reports"));
    QVERIFY(reopened.remove(QStringLiteral("c1")));
    QCOMPARE(reopened.chains().size(), 1);
    QVERIFY(!reopened.remove(QStringLiteral("nothing here")));
}

void TestChain::aChainThatWillNotLoadIsDroppedAndCounted()
{
    // One bad chain must not take the other nine with it -- and it must not
    // vanish in silence either, because a scheduled chain that stops happening
    // with nothing to read about it is the worst of both.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("chains.json"));
    Chain good;
    good.id = QStringLiteral("good");
    good.steps = { stepOf(QStringLiteral("search")) };
    Chain bad;
    bad.id = QStringLiteral("bad");
    bad.steps = { stepOf(QStringLiteral("search")) };

    QJsonObject root;
    QJsonArray chains;
    chains.append(good.toJson());
    QJsonObject broken = bad.toJson();
    QJsonArray steps = broken.value(QStringLiteral("steps")).toArray();
    QJsonObject step = steps.at(0).toObject();
    step.remove(QStringLiteral("kind"));
    steps.replace(0, step);
    broken[QStringLiteral("steps")] = steps;
    chains.append(broken);
    root[QStringLiteral("chains")] = chains;

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();

    ChainStore store(path);
    QVERIFY(store.load());
    QCOMPARE(store.chains().size(), 1);
    QCOMPARE(store.chains().first().id, QStringLiteral("good"));
    QCOMPARE(store.unreadable(), 1);
}

MOLE_TEST_MAIN(TestChain)

#include "tst_Chain.moc"
