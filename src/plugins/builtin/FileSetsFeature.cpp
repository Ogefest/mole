#include "plugins/builtin/FileSetsFeature.h"

#include "core/events/EventBus.h"
#include "core/sets/VerifySetTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

namespace mole {

FileSetsController::FileSetsController(PluginServices services, FileSetStore* store, QObject* parent)
    : FeatureController(QStringLiteral("Sets"), parent)
    , m_services(services)
    , m_store(store)
{
    if (m_store)
        connect(m_store, &FileSetStore::setsChanged, this, &FileSetsController::refresh);
    refresh();
}

FileSetsController::~FileSetsController() = default;

void FileSetsController::refresh()
{
    if (m_store && !m_currentId.isEmpty() && !m_store->set(m_currentId).isValid())
        m_currentId.clear();
    if (m_store && m_currentId.isEmpty() && !m_store->sets().isEmpty())
        m_currentId = m_store->sets().first().id;

    setTitle(currentName().isEmpty() ? QStringLiteral("Sets") : currentName());
    setSubtitle(m_store && !m_currentId.isEmpty()
            ? QStringLiteral("%1 items").arg(memberCount())
            : QStringLiteral("%1 sets").arg(m_store ? m_store->sets().size() : 0));

    emit setsChanged();
    emit currentChanged();
    emit membersChanged();
}

QVariantList FileSetsController::sets() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const QList<FileSet> all = m_store->sets();
    for (const FileSet& set : all) {
        out.append(QVariantMap { { QStringLiteral("id"), set.id }, { QStringLiteral("name"), set.name },
            { QStringLiteral("count"), set.count() }, { QStringLiteral("driveCount"), set.driveCount() },
            { QStringLiteral("current"), set.id == m_currentId } });
    }
    return out;
}

void FileSetsController::setCurrentSetId(const QString& id)
{
    if (m_currentId == id)
        return;
    m_currentId = id;
    // Presence belongs to the set that was checked, not to the tab.
    m_present.clear();
    m_sizes.clear();
    refresh();
    emit stateChanged();
}

QString FileSetsController::currentName() const
{
    return m_store ? m_store->set(m_currentId).name : QString();
}

QList<VfsUri> FileSetsController::targets() const
{
    return m_store ? m_store->set(m_currentId).targets() : QList<VfsUri> {};
}

QStringList FileSetsController::targetUris() const
{
    QStringList out;
    const QList<VfsUri> uris = targets();
    out.reserve(uris.size());
    for (const VfsUri& uri : uris)
        out.append(uri.toString());
    return out;
}

int FileSetsController::memberCount() const
{
    return m_store ? m_store->set(m_currentId).count() : 0;
}

int FileSetsController::missingCount() const
{
    int missing = 0;
    for (auto it = m_present.constBegin(); it != m_present.constEnd(); ++it) {
        if (!it.value())
            ++missing;
    }
    return missing;
}

QString FileSetsController::summary() const
{
    if (!m_store || m_currentId.isEmpty())
        return {};

    const FileSet set = m_store->set(m_currentId);
    QString text = QStringLiteral("%1 items").arg(set.count());
    if (set.driveCount() > 1)
        text += QStringLiteral(" across %1 drives").arg(set.driveCount());

    qint64 bytes = 0;
    for (auto it = m_sizes.constBegin(); it != m_sizes.constEnd(); ++it)
        bytes += it.value();
    if (bytes > 0)
        text += QStringLiteral(" · %1").arg(QLocale().formattedDataSize(bytes));
    if (missingCount() > 0)
        text += QStringLiteral(" · %1 missing").arg(missingCount());
    return text;
}

QVariantList FileSetsController::members() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const FileSet set = m_store->set(m_currentId);
    for (const QString& uri : set.uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (!m_filter.isEmpty() && !uri.contains(m_filter, Qt::CaseInsensitive))
            continue;

        const bool checked = m_present.contains(uri);
        out.append(
            QVariantMap { { QStringLiteral("uri"), uri }, { QStringLiteral("name"), parsed.fileName() },
                { QStringLiteral("location"), parsed.parent().toString() },
                { QStringLiteral("drive"), parsed.scheme() }, { QStringLiteral("checked"), checked },
                // "Not checked yet" and "not there" are different states and are
                // shown differently; conflating them would report a healthy set as
                // broken before anything had looked.
                { QStringLiteral("missing"), checked && !m_present.value(uri) },
                { QStringLiteral("sizeText"),
                    m_sizes.contains(uri) ? QLocale().formattedDataSize(m_sizes.value(uri)) : QString() } });
    }
    return out;
}

void FileSetsController::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    emit membersChanged();
}

QString FileSetsController::createSet(const QString& name)
{
    if (!m_store)
        return {};
    const FileSet set = m_store->create(name);
    if (!set.isValid())
        return {};
    setCurrentSetId(set.id);
    return set.id;
}

bool FileSetsController::renameSet(const QString& id, const QString& name)
{
    return m_store && m_store->rename(id, name);
}

bool FileSetsController::removeSet(const QString& id)
{
    return m_store && m_store->remove(id);
}

int FileSetsController::addUris(const QStringList& uris)
{
    if (!m_store || m_currentId.isEmpty())
        return 0;
    const int added = m_store->addTo(m_currentId, uris);
    if (added > 0)
        emit stateChanged();
    return added;
}

int FileSetsController::removeUris(const QStringList& uris)
{
    if (!m_store || m_currentId.isEmpty())
        return 0;
    for (const QString& uri : uris) {
        m_present.remove(uri);
        m_sizes.remove(uri);
    }
    const int removed = m_store->removeFrom(m_currentId, uris);
    if (removed > 0)
        emit stateChanged();
    return removed;
}

int FileSetsController::forgetMissing()
{
    QStringList gone;
    for (auto it = m_present.constBegin(); it != m_present.constEnd(); ++it) {
        if (!it.value())
            gone.append(it.key());
    }
    return gone.isEmpty() ? 0 : removeUris(gone);
}

void FileSetsController::verify()
{
    if (!m_services.isValid() || !m_store)
        return;

    // Statting each member touches storage, so it goes through the task layer
    // like everything else that does. One task for the whole set rather than
    // one per member: a set of five hundred files would otherwise flood the
    // queue with work too small to be worth scheduling.
    const FileSet set = m_store->set(m_currentId);
    if (set.uris.isEmpty())
        return;

    VfsManager* vfs = m_services.vfs;
    const QStringList uris = set.uris;

    auto* task = new VerifySetTask(vfs, uris);
    connect(task, &VerifySetTask::verified, this,
        [this](const QHash<QString, bool>& present, const QHash<QString, qint64>& sizes) {
            m_present = present;
            m_sizes = sizes;
            emit membersChanged();
        });
    m_services.tasks->submit(task);
}

void FileSetsController::reportMissing(const QString& uri)
{
    if (!m_services.events)
        return;

    const VfsUri parsed = VfsUri::fromString(uri);
    m_services.events->postNotification(EventBus::Severity::Warning,
        QStringLiteral("%1 is not there any more").arg(parsed.fileName()),
        QStringLiteral("The set still remembers it. Use Forget missing to drop it, or put the file "
                       "back at %1.")
            .arg(parsed.parent().toString()));
}

QVariantMap FileSetsController::saveState() const
{
    return { { QStringLiteral("currentSetId"), m_currentId }, { QStringLiteral("filter"), m_filter } };
}

void FileSetsController::restoreState(const QVariantMap& state)
{
    setFilter(state.value(QStringLiteral("filter")).toString());
    const QString id = state.value(QStringLiteral("currentSetId")).toString();
    if (!id.isEmpty())
        setCurrentSetId(id);
}

FileSetsFeature::FileSetsFeature(PluginServices services, FileSetStore* store)
    : m_services(services)
    , m_store(store)
{
}

QUrl FileSetsFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/FileSetsView.qml"));
}

FeatureController* FileSetsFeature::createController(QObject* parent)
{
    return new FileSetsController(m_services, m_store, parent);
}

} // namespace mole
