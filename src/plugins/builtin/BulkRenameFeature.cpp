#include "plugins/builtin/BulkRenameFeature.h"

#include "core/events/EventBus.h"
#include "core/rename/RenameTask.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace mole {

BulkRenameController::BulkRenameController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Bulk rename"), parent)
    , m_services(services)
{
}

BulkRenameController::~BulkRenameController()
{
    if (m_task)
        m_task->requestCancel();
}

QStringList BulkRenameController::sourceUris() const
{
    QStringList out;
    out.reserve(m_sources.size());
    for (const VfsUri& uri : m_sources)
        out.append(uri.toString());
    return out;
}

void BulkRenameController::setTargets(const QStringList& uris)
{
    m_sources.clear();
    for (const QString& uri : uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (parsed.isValid())
            m_sources.append(parsed);
    }

    setSubtitle(QStringLiteral("%1 files").arg(m_sources.size()));
    refreshDirectoryContents();
    emit sourcesChanged();
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::refreshDirectoryContents()
{
    m_existing.clear();
    // A listing for a selection nobody is looking at any more is an answer that
    // would arrive over the top of the next one.
    for (const QPointer<Task>& pending : std::as_const(m_pendingListings)) {
        if (pending)
            pending->requestCancel();
    }
    m_pendingListings.clear();
    if (!m_services.isValid())
        return;

    // Read once, when the targets are set. A listing per keystroke would make
    // the preview stutter, and the directory is not going to change while
    // somebody is composing rules -- and if it does, the rename itself reports
    // the failure rather than silently overwriting.
    QSet<QString> directories;
    for (const VfsUri& source : std::as_const(m_sources))
        directories.insert(source.parent().toString());

    // Whatever the backend calls a collision, and whatever it will not accept in
    // a name, is what the preview has to predict -- so both answers come from
    // the drive rather than from the uri's scheme or from this machine.
    m_caseSensitivity = Qt::CaseSensitive;
    m_nameRules.clear();

    for (const QString& directory : directories) {
        const VfsUri uri = VfsUri::fromString(directory);
        FileSystemPtr fs = m_services.vfs->resolve(uri);
        if (!fs)
            continue;
        // Two questions the drive answers without going anywhere -- they are
        // properties of the backend, not of the folder -- so they stay here.
        if (fs->pathCaseSensitivity() == Qt::CaseInsensitive)
            m_caseSensitivity = Qt::CaseInsensitive;
        // Per directory. This used to assign one member, so a selection spanning
        // a local disk and an SMB share was previewed with whichever drive the
        // hash-ordered last directory happened to be on -- either inventing
        // refusals for the local files or missing them for the share's.
        // See MOLE-377.
        m_nameRules.insert(directory, fs->nameRules());

        // The listing is the part that goes to storage, and it used to be made
        // from the thread that draws -- once per distinct parent folder, every
        // time the selection changed. Selecting two hundred files across four
        // folders on a share that has stopped answering stopped the window four
        // times over. See ARCHITECTURE.md's first rule and MOLE-360.
        if (!m_services.tasks)
            continue;
        auto* task = new ListDirectoryTask(fs, uri);
        connect(task, &ListDirectoryTask::listed, this,
            [this, directory](const mole::VfsUri&, const mole::FileEntryList& entries) {
                QStringList names;
                names.reserve(entries.size());
                for (const FileEntry& entry : entries)
                    names.append(entry.name);
                m_existing.insert(directory, names);
                // The preview is what the names are for: a collision this
                // listing reveals has to appear in it, so it is worked out again
                // now the answer is here.
                rebuildPreview();
                emit stateChanged();
            });
        m_pendingListings.append(task);
        m_services.tasks->submit(task);
    }
}

QVariantList BulkRenameController::ruleKinds() const
{
    QVariantList out;
    for (RenameRule::Kind kind : RenameRule::allKinds()) {
        out.append(QVariantMap { { QStringLiteral("id"), RenameRule::kindToString(kind) },
            { QStringLiteral("label"), RenameRule::kindLabel(kind) } });
    }
    return out;
}

QVariantList BulkRenameController::rules() const
{
    QVariantList out;
    for (int i = 0; i < m_rules.size(); ++i) {
        const RenameRule& rule = m_rules.at(i);
        QVariantMap row = rule.toJson().toVariantMap();
        row.insert(QStringLiteral("index"), i);
        row.insert(QStringLiteral("label"), RenameRule::kindLabel(rule.kind));
        row.insert(QStringLiteral("description"), rule.describe());
        out.append(row);
    }
    return out;
}

QVariantList BulkRenameController::preview() const
{
    QVariantList out;
    const QList<RenamePlan::Entry>& entries = m_plan.entries();
    for (const RenamePlan::Entry& entry : entries) {
        out.append(QVariantMap { { QStringLiteral("uri"), entry.source.toString() },
            { QStringLiteral("from"), entry.originalName }, { QStringLiteral("to"), entry.newName },
            { QStringLiteral("changed"), entry.changed() }, { QStringLiteral("blocked"), entry.isBlocked() },
            { QStringLiteral("problem"), entry.problem } });
    }
    return out;
}

bool BulkRenameController::canApply() const
{
    return m_plan.canApply() && !m_task;
}

QString BulkRenameController::summary() const
{
    if (m_sources.isEmpty())
        return {};
    if (m_rules.isEmpty())
        return QStringLiteral("%1 files · add a rule to see what would change").arg(m_sources.size());

    QString text = QStringLiteral("%1 of %2 would change").arg(m_plan.changedCount()).arg(m_sources.size());
    if (m_plan.blockedCount() > 0) {
        text += QStringLiteral("  ·  %1 cannot be renamed").arg(m_plan.blockedCount());
    }
    return text;
}

void BulkRenameController::rebuildPreview()
{
    m_plan = RenamePlan::build(m_sources, m_rules, m_existing, m_caseSensitivity, m_nameRules);
    emit previewChanged();
}

void BulkRenameController::addRule(const QString& kind)
{
    RenameRule rule;
    rule.kind = RenameRule::kindFromString(kind);
    m_rules.append(rule);
    emit rulesChanged();
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::removeRule(int index)
{
    if (index < 0 || index >= m_rules.size())
        return;
    m_rules.removeAt(index);
    emit rulesChanged();
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::moveRule(int index, int delta)
{
    const int target = index + delta;
    if (index < 0 || index >= m_rules.size() || target < 0 || target >= m_rules.size())
        return;
    // Order is meaning here: stripping digits before numbering is a different
    // result from numbering before stripping, and both are legitimate.
    m_rules.move(index, target);
    emit rulesChanged();
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::setRuleEnabled(int index, bool enabled)
{
    if (index < 0 || index >= m_rules.size())
        return;
    m_rules[index].enabled = enabled;
    emit rulesChanged();
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::setRuleField(int index, const QString& field, const QVariant& value)
{
    if (index < 0 || index >= m_rules.size())
        return;

    // Round-tripped through the rule's own serialisation, so the form needs no
    // setter per field and a new field on a rule needs no code here at all.
    QJsonObject json = m_rules.at(index).toJson();
    json[field] = QJsonValue::fromVariant(value);
    m_rules[index] = RenameRule::fromJson(json);

    // Deliberately not emitting rulesChanged(). The form is the only thing that
    // reads the rules, and it is where this change came from -- telling it that
    // the list has changed makes its Repeater rebuild the delegates, which
    // destroys the very field being typed into. With the notification in place,
    // typing "2024_" into a prefix left "2": the first character round-tripped,
    // the field was replaced, and the rest went nowhere. The preview is what
    // needs to follow the keystroke, and previewChanged() says so.
    rebuildPreview();
    emit stateChanged();
}

void BulkRenameController::apply()
{
    if (!canApply() || !m_services.isValid())
        return;

    QList<RenamePlan::Entry> doable;
    for (const RenamePlan::Entry& entry : m_plan.entries()) {
        if (entry.changed() && !entry.isBlocked())
            doable.append(entry);
    }
    if (doable.isEmpty())
        return;

    setErrorText(QString());

    auto* task = new RenameTask(m_services.vfs, doable);
    m_task = task;
    setBusy(true);

    QSet<QString> touched;
    for (const RenamePlan::Entry& entry : doable)
        touched.insert(entry.source.parent().toString());

    connect(task, &Task::finished, this, [this, task, touched] {
        if (m_task != task)
            return;
        m_task.clear();
        setBusy(false);

        // What actually happened, rather than what was planned. This used to
        // rebuild the list as though every doable rename had succeeded, clear
        // the rules and say nothing -- so after a partial failure the tab listed
        // names that were never created and had lost the files that still
        // carried their old ones. RenameTask has always reported its failures
        // and nothing read them. See MOLE-377.
        // Held in a local before the set is built from it: begin() and end() on
        // a function returning by value are iterators into two different
        // temporaries.
        const QStringList renamed = task->renamedSources();
        const QSet<QString> moved(renamed.begin(), renamed.end());
        const QStringList failures = task->failures();

        QStringList aimAt;
        for (const RenamePlan::Entry& entry : m_plan.entries()) {
            const QString source = entry.source.toString();
            aimAt.append(
                moved.contains(source) ? entry.source.parent().child(entry.newName).toString() : source);
        }

        if (m_services.events) {
            for (const QString& directory : touched)
                m_services.events->postDirectoryChanged(VfsUri::fromString(directory));
        }

        // The rules are cleared only when there is nothing left to do with them.
        // Somebody whose batch half-failed wants the rules that produced it, so
        // they can fix the cause and press the button again.
        if (failures.isEmpty()) {
            m_rules.clear();
            emit rulesChanged();
        } else if (task->state() == Task::State::Failed) {
            setErrorText(task->error().message);
        } else {
            setErrorText(failures.size() == 1 ? failures.first()
                                              : QStringLiteral("%1 of %2 renames failed — %3")
                                                    .arg(failures.size())
                                                    .arg(renamed.size() + failures.size())
                                                    .arg(failures.join(QStringLiteral("; "))));
        }
        setTargets(aimAt);
    });

    m_services.tasks->submit(task);
    emit previewChanged();
}

void BulkRenameController::setErrorText(const QString& text)
{
    if (m_errorText == text)
        return;
    m_errorText = text;
    emit errorTextChanged();
}

QVariantMap BulkRenameController::saveState() const
{
    QJsonArray rules;
    for (const RenameRule& rule : m_rules)
        rules.append(rule.toJson());

    return { { QStringLiteral("sources"), sourceUris() },
        { QStringLiteral("rules"), QString::fromUtf8(QJsonDocument(rules).toJson(QJsonDocument::Compact)) } };
}

void BulkRenameController::restoreState(const QVariantMap& state)
{
    const QJsonDocument document
        = QJsonDocument::fromJson(state.value(QStringLiteral("rules")).toString().toUtf8());
    const QJsonArray rules = document.array();
    for (const QJsonValue& value : rules)
        m_rules.append(RenameRule::fromJson(value.toObject()));

    emit rulesChanged();
    setTargets(state.value(QStringLiteral("sources")).toStringList());
}

BulkRenameFeature::BulkRenameFeature(PluginServices services)
    : m_services(services)
{
}

QUrl BulkRenameFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/BulkRenameView.qml"));
}

FeatureController* BulkRenameFeature::createController(QObject* parent)
{
    return new BulkRenameController(m_services, parent);
}

} // namespace mole
