#include "plugins/builtin/SyncFeature.h"

#include "core/events/EventBus.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

namespace mole {

SyncController::SyncController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Sync"), parent)
    , m_services(services)
{
}

SyncController::~SyncController()
{
    if (m_task)
        m_task->requestCancel();
}

bool SyncController::isReady() const
{
    return !m_source.isEmpty() && !m_target.isEmpty() && m_source != m_target;
}

void SyncController::setSourceUri(const QString& uri)
{
    if (m_source == uri)
        return;
    m_source = uri;
    // A plan describes two particular folders. Keeping it after one of them
    // changes would offer to carry out something nobody asked for.
    m_hasPlan = false;
    m_plan = SyncPlan();
    setSubtitle(VfsUri::fromString(m_source).fileName());
    emit endpointsChanged();
    emit planChanged();
    emit stateChanged();
}

void SyncController::setTargetUri(const QString& uri)
{
    if (m_target == uri)
        return;
    m_target = uri;
    m_hasPlan = false;
    m_plan = SyncPlan();
    emit endpointsChanged();
    emit planChanged();
    emit stateChanged();
}

void SyncController::setTargets(const QStringList& uris)
{
    if (uris.isEmpty())
        return;
    setSourceUri(uris.first());
    // Two folders selected is a plain reading of "sync these": the second is
    // where the first should end up.
    if (uris.size() > 1)
        setTargetUri(uris.at(1));
}

void SyncController::swapEnds()
{
    const QString source = m_source;
    setSourceUri(m_target);
    setTargetUri(source);
}

QVariantList SyncController::modes() const
{
    QVariantList out;
    for (SyncOptions::Mode mode :
        { SyncOptions::Mode::Update, SyncOptions::Mode::Mirror, SyncOptions::Mode::FillGaps }) {
        out.append(QVariantMap { { QStringLiteral("id"), SyncOptions::modeToString(mode) },
            { QStringLiteral("label"), SyncOptions::modeLabel(mode) },
            { QStringLiteral("description"), SyncOptions::modeDescription(mode) } });
    }
    return out;
}

QVariantList SyncController::compareChoices() const
{
    QVariantList out;
    for (SyncOptions::Compare compare : { SyncOptions::Compare::SizeAndTime, SyncOptions::Compare::SizeOnly,
             SyncOptions::Compare::Contents }) {
        out.append(QVariantMap { { QStringLiteral("id"), SyncOptions::compareToString(compare) },
            { QStringLiteral("label"), SyncOptions::compareLabel(compare) } });
    }
    return out;
}

QString SyncController::mode() const
{
    return SyncOptions::modeToString(m_options.mode);
}

void SyncController::setMode(const QString& mode)
{
    const SyncOptions::Mode wanted = SyncOptions::modeFromString(mode);
    if (m_options.mode == wanted)
        return;
    m_options.mode = wanted;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
    emit stateChanged();
}

QString SyncController::modeDescription() const
{
    return SyncOptions::modeDescription(m_options.mode);
}

QString SyncController::compare() const
{
    return SyncOptions::compareToString(m_options.compare);
}

void SyncController::setCompare(const QString& compare)
{
    const SyncOptions::Compare wanted = SyncOptions::compareFromString(compare);
    if (m_options.compare == wanted)
        return;
    m_options.compare = wanted;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
    emit stateChanged();
}

void SyncController::setSkipNewer(bool skip)
{
    if (m_options.skipNewer == skip)
        return;
    m_options.skipNewer = skip;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
}

void SyncController::setRecursive(bool recursive)
{
    if (m_options.recursive == recursive)
        return;
    m_options.recursive = recursive;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
}

void SyncController::setIncludeHidden(bool include)
{
    if (m_options.includeHidden == include)
        return;
    m_options.includeHidden = include;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
}

void SyncController::setIncludePatterns(const QString& patterns)
{
    const QStringList wanted = patterns.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (m_options.includePatterns == wanted)
        return;
    m_options.includePatterns = wanted;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
}

void SyncController::setExcludePatterns(const QString& patterns)
{
    const QStringList wanted = patterns.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (m_options.excludePatterns == wanted)
        return;
    m_options.excludePatterns = wanted;
    m_hasPlan = false;
    emit optionsChanged();
    emit planChanged();
}

int SyncController::deleteCount() const
{
    return m_plan.countOf(SyncPlan::Action::Delete);
}

QVariantList SyncController::deletions() const
{
    QVariantList out;
    const QLocale locale;
    for (const SyncPlan::Step& step : m_plan.steps()) {
        if (step.action != SyncPlan::Action::Delete)
            continue;
        out.append(
            QVariantMap { { QStringLiteral("name"), step.relativePath }, { QStringLiteral("isDir"), false },
                { QStringLiteral("detail"),
                    step.bytes > 0 ? locale.formattedDataSize(step.bytes) : QString() } });
    }
    return out;
}

QVariantList SyncController::steps() const
{
    QVariantList out;
    const QLocale locale;
    const QList<SyncPlan::Step>& steps = m_plan.steps();
    for (const SyncPlan::Step& step : steps) {
        out.append(QVariantMap { { QStringLiteral("action"), SyncPlan::actionLabel(step.action) },
            { QStringLiteral("path"), step.relativePath }, { QStringLiteral("reason"), step.reason },
            { QStringLiteral("sizeText"), step.bytes > 0 ? locale.formattedDataSize(step.bytes) : QString() },
            { QStringLiteral("destructive"),
                step.action == SyncPlan::Action::Delete || step.action == SyncPlan::Action::Overwrite },
            { QStringLiteral("skipped"), step.action == SyncPlan::Action::Skip } });
    }
    return out;
}

QString SyncController::planSummary() const
{
    if (isRunning())
        return QStringLiteral("working…");
    if (!m_hasPlan)
        return {};

    const QLocale locale;
    const int copies = m_plan.countOf(SyncPlan::Action::Copy);
    const int replaces = m_plan.countOf(SyncPlan::Action::Overwrite);
    const int deletes = m_plan.countOf(SyncPlan::Action::Delete);
    const int folders = m_plan.countOf(SyncPlan::Action::CreateDirectory);

    if (copies + replaces + deletes + folders == 0)
        return QStringLiteral("already in step");

    QStringList parts;
    if (folders > 0)
        parts.append(QStringLiteral("%1 new folders").arg(folders));
    if (copies > 0)
        parts.append(QStringLiteral("%1 to copy").arg(copies));
    if (replaces > 0)
        parts.append(QStringLiteral("%1 to replace").arg(replaces));
    if (deletes > 0)
        parts.append(QStringLiteral("%1 to delete").arg(deletes));

    QString text = parts.join(QStringLiteral(" · "));
    if (m_plan.bytesToTransfer() > 0)
        text += QStringLiteral(" · %1").arg(locale.formattedDataSize(m_plan.bytesToTransfer()));
    if (m_lastWasDryRun)
        text += QStringLiteral("  (nothing written yet)");
    return text;
}

void SyncController::start(bool dryRun)
{
    if (!isReady() || !m_services.isValid() || m_task)
        return;

    const VfsUri source = VfsUri::fromString(m_source);
    const VfsUri target = VfsUri::fromString(m_target);
    FileSystemPtr sourceFs = m_services.vfs->resolve(source);
    FileSystemPtr targetFs = m_services.vfs->resolve(target);
    if (!sourceFs || !targetFs)
        return;

    SyncOptions options = m_options;
    options.dryRun = dryRun;
    m_lastWasDryRun = dryRun;

    auto* task = new SyncTask(std::move(sourceFs), source, std::move(targetFs), target, options);
    m_task = task;
    setBusy(true);
    m_hasPlan = false;

    connect(task, &SyncTask::planReady, this, [this](const SyncPlan& plan) {
        m_plan = plan;
        m_hasPlan = true;
        emit planChanged();
    });
    connect(task, &Task::statusTextChanged, this, [this, task] {
        if (m_task != task)
            return;
        m_progressText = task->statusText();
        emit planChanged();
    });
    connect(task, &Task::finished, this, [this, task, target] {
        if (m_task != task)
            return;
        m_task.clear();
        setBusy(false);
        m_progressText = task->statusText();

        if (!task->failures().isEmpty())
            m_errorText = task->failures().join(QLatin1String("; "));

        // Whoever is showing the destination should see the result.
        if (m_services.events && !m_lastWasDryRun)
            m_services.events->postDirectoryChanged(target);

        emit planChanged();
    });

    m_services.tasks->submit(task);
    emit planChanged();
}

void SyncController::preview()
{
    m_errorText.clear();
    start(true);
}

void SyncController::apply()
{
    m_errorText.clear();
    start(false);
}

void SyncController::cancel()
{
    if (m_task)
        m_task->requestCancel();
}

QVariantMap SyncController::saveState() const
{
    QVariantMap state = m_options.toJson().toVariantMap();
    state.insert(QStringLiteral("source"), m_source);
    state.insert(QStringLiteral("target"), m_target);
    return state;
}

void SyncController::restoreState(const QVariantMap& state)
{
    m_options = SyncOptions::fromJson(QJsonObject::fromVariantMap(state));
    // Never restored as anything but a dry run: an application that reopened
    // into a live mirror would be one bad restore away from deleting a tree.
    m_options.dryRun = true;
    setSourceUri(state.value(QStringLiteral("source")).toString());
    setTargetUri(state.value(QStringLiteral("target")).toString());
    emit optionsChanged();
}

SyncFeature::SyncFeature(PluginServices services)
    : m_services(services)
{
}

QUrl SyncFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/SyncView.qml"));
}

FeatureController* SyncFeature::createController(QObject* parent)
{
    return new SyncController(m_services, parent);
}

} // namespace mole
