#include "plugins/builtin/DuplicatesFeature.h"

#include "core/duplicates/Strategies.h"
#include "core/events/EventBus.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

namespace mole {
namespace {

    std::unique_ptr<IDuplicateStrategy> strategyById(const QString& id)
    {
        auto all = IDuplicateStrategy::all();
        for (auto& strategy : all) {
            if (strategy->id() == id)
                return std::move(strategy);
        }
        return std::make_unique<SameContentStrategy>();
    }

} // namespace

DuplicatesController::DuplicatesController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Duplicates"), parent)
    , m_services(services)
    , m_groups(new DuplicateGroupModel(this))
{
}

DuplicatesController::~DuplicatesController()
{
    if (m_task)
        m_task->requestCancel();
}

void DuplicatesController::setTargets(const QStringList& uris)
{
    // Folders are what a duplicate scan searches. Handed a selection of files,
    // the folders they live in are the sensible reading -- looking for
    // duplicates "of these three files" is a different question.
    QStringList roots;
    for (const QString& uri : uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (!parsed.isValid())
            continue;
        const QString root = parsed.toString();
        if (!roots.contains(root))
            roots.append(root);
    }

    m_roots = roots;
    setSubtitle(m_roots.size() == 1 ? VfsUri::fromString(m_roots.first()).fileName()
                                    : QStringLiteral("%1 folders").arg(m_roots.size()));
    emit rootsChanged();
    emit stateChanged();
}

QVariantList DuplicatesController::strategies() const
{
    QVariantList out;
    const auto all = IDuplicateStrategy::all();
    for (const auto& strategy : all) {
        out.append(QVariantMap { { QStringLiteral("id"), strategy->id() },
            { QStringLiteral("label"), strategy->label() },
            { QStringLiteral("description"), strategy->description() },
            { QStringLiteral("stages"), strategy->stageNames() } });
    }
    return out;
}

QString DuplicatesController::strategyDescription() const
{
    return strategyById(m_strategyId)->description();
}

void DuplicatesController::setStrategyId(const QString& id)
{
    if (m_strategyId == id)
        return;
    m_strategyId = id;
    emit optionsChanged();
    emit stateChanged();
}

void DuplicatesController::setMinimumSize(qint64 bytes)
{
    if (m_minimumSize == bytes)
        return;
    m_minimumSize = std::max<qint64>(0, bytes);
    emit optionsChanged();
    emit stateChanged();
}

QString DuplicatesController::summary() const
{
    // Both figures are kept by the model as they change. Walking every group for
    // them was cheap on its own and ruinous where it sat: this is read on every
    // confirmation, and there may be five hundred groups by the end.
    const QString found = QStringLiteral("%1 groups · %2 could be freed")
                              .arg(m_groups->rowCount())
                              .arg(QLocale().formattedDataSize(m_groups->reclaimableBytes()));

    // Groups arrive as they are confirmed, so a scan in flight has something to
    // say about itself beyond "scanning…" -- and what it has found so far is
    // already on screen, which would make a summary that ignored it read wrong.
    if (isScanning())
        return m_groups->rowCount() == 0 ? QStringLiteral("scanning…") : found + QStringLiteral(" so far");
    if (!m_hasRun)
        return {};
    // A stopped scan says so. What it found is kept -- every group of it agreed
    // at every stage -- but "no duplicates found" about a scan that was cut off
    // after ten seconds of a ten-minute tree is not true.
    if (m_wasCancelled) {
        return m_groups->rowCount() == 0 ? QStringLiteral("stopped before anything was found")
                                         : found + QStringLiteral(" · stopped early");
    }
    QString said = m_groups->rowCount() == 0 ? QStringLiteral("no duplicates found") : found;
    // What the walk could not read, in the same sentence as the answer -- because
    // it qualifies the answer. "no duplicates found" about a tree whose largest
    // subtree could not be opened is a statement about permissions dressed up as
    // a statement about the files. See ADR-0030 and MOLE-341.
    if (m_unreadable > 0)
        said += QStringLiteral(" · %1 place(s) could not be read").arg(m_unreadable);
    if (m_links > 0)
        said += QStringLiteral(" · %1 link(s) left out").arg(m_links);
    if (!m_deleteFailures.isEmpty()) {
        said += QStringLiteral(" · %1 could not be deleted: %2")
                    .arg(m_deleteFailures.size())
                    .arg(m_deleteFailures.join(QStringLiteral("; ")));
    }
    return said;
}

QStringList DuplicatesController::selectedUris() const
{
    return m_groups->selectedUris();
}

QVariantList DuplicatesController::selectedDetails() const
{
    QVariantList out;
    const QLocale locale;
    for (const DuplicateGroup& group : m_groups->groups()) {
        for (const FileEntry& entry : group.files) {
            if (!m_groups->isSelected(entry.uri.toString()))
                continue;
            out.append(QVariantMap { { QStringLiteral("name"), entry.uri.toString() },
                // Named separately from the text shown, because the dialog these
                // rows go into is what then has to delete exactly these rows.
                // See deleteSelected() and MOLE-339.
                { QStringLiteral("uri"), entry.uri.toString() },
                { QStringLiteral("isDir"), false },
                { QStringLiteral("detail"), locale.formattedDataSize(entry.size) } });
        }
    }
    return out;
}

int DuplicatesController::copyCount() const
{
    return m_groups->copyCount();
}

void DuplicatesController::setRuleText(const QString& text)
{
    if (m_ruleText == text)
        return;
    m_ruleText = text;
    // Announced with the selection rather than on its own: they always change
    // together, and two signals would let a view draw a rule beside a count that
    // no longer came from it.
}

QString DuplicatesController::selectedSizeText() const
{
    return QLocale().formattedDataSize(m_groups->selectedBytes());
}

void DuplicatesController::scan()
{
    if (!m_services.isValid() || m_roots.isEmpty() || m_task)
        return;

    QList<VfsUri> roots;
    for (const QString& uri : std::as_const(m_roots))
        roots.append(VfsUri::fromString(uri));

    m_groups->clear();
    m_wasCancelled = false;
    m_ruleText.clear();
    m_progressText.clear();
    emit resultsChanged();
    emit selectionChanged();
    emit progressChanged();

    auto* task = new FindDuplicatesTask(m_services.vfs, roots, strategyById(m_strategyId));
    task->setMinimumSize(m_minimumSize);
    m_task = task;
    setBusy(true);

    // Groups as the scan confirms them, each in the place the task worked out for
    // it -- so the list is sorted largest-first at every instant rather than only
    // once the walk has finished. See ADR-0043.
    //
    // In whatever batches the task's drain hands over, which is what bounds how
    // many events a burst of confirmations can put in front of the window. The
    // positions are applied in the order they were given: each is where its group
    // belongs in a list that already holds the ones before it.
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [this, task](const QList<DuplicateGroup>& groups, const QList<int>& positions) {
            if (m_task != task)
                return;
            for (int i = 0; i < groups.size() && i < positions.size(); ++i) {
                // An insertion, announced as one. The scalar properties -- the
                // count, the summary -- are read again off resultsChanged and
                // cost nothing; what used to cost was that the list was read
                // again with them.
                m_groups->insertGroup(groups.at(i), positions.at(i));
            }
            emit resultsChanged();
        });
    connect(task, &Task::statusTextChanged, this, [this, task] {
        if (m_task != task)
            return;
        m_progressText = task->statusText();
        emit progressChanged();
    });
    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setBusy(false);
        m_hasRun = true;
        // Whatever was confirmed before the stop stays. A cancelled walk is not a
        // wrong answer, only a short one, and the alternative -- throwing away
        // groups somebody is already looking at -- is the surprising behaviour.
        m_wasCancelled = task->state() == Task::State::Cancelled;
        m_unreadable = task->unreadablePlaces();
        m_links = task->linksLeftOut();
        m_deleteFailures.clear();
        m_progressText.clear();
        emit progressChanged();
        emit resultsChanged();
        emit selectionChanged();
    });

    m_services.tasks->submit(task);
    emit resultsChanged();
}

void DuplicatesController::cancel()
{
    if (m_task)
        m_task->requestCancel();
}

void DuplicatesController::toggle(const QString& uri)
{
    // One row changes and the model says so. No resultsChanged: nothing about
    // the results changed, and it used to be what rebuilt all of them.
    m_groups->toggle(uri);
    // A rule that has been edited is no longer the rule. Saying "keeping the
    // newest" over ticks somebody has since changed by hand would be the view
    // asserting something untrue about what will be deleted.
    setRuleText(m_groups->selectedCount() == 0 ? QString {} : QStringLiteral("Chosen by hand"));
    emit selectionChanged();
}

void DuplicatesController::keepOnly(const QString& uri)
{
    // The group this copy belongs to, and only that one: the point of a per-group
    // override is that the other forty-nine keep whatever they were given -- and
    // the model announces the one row, so they are not even redrawn.
    m_groups->keepOnly(uri);
    setRuleText(QStringLiteral("Chosen by hand"));
    emit selectionChanged();
}

void DuplicatesController::clearSelection()
{
    m_groups->clearSelection();
    setRuleText({});
    emit selectionChanged();
}

void DuplicatesController::selectAllBut(
    const QString& rule, const std::function<int(const QList<FileEntry>&)>& chooseKeeper)
{
    m_groups->selectAllBut(chooseKeeper);
    setRuleText(m_groups->rowCount() == 0 ? QString {} : rule);
    emit selectionChanged();
}

void DuplicatesController::keepNewest()
{
    selectAllBut(QStringLiteral("Keeping the newest of each group"), [](const QList<FileEntry>& files) {
        int best = 0;
        for (int i = 1; i < files.size(); ++i) {
            if (files.at(i).modified > files.at(best).modified)
                best = i;
        }
        return best;
    });
}

void DuplicatesController::keepOldest()
{
    selectAllBut(QStringLiteral("Keeping the oldest of each group"), [](const QList<FileEntry>& files) {
        int best = 0;
        for (int i = 1; i < files.size(); ++i) {
            if (files.at(i).modified < files.at(best).modified)
                best = i;
        }
        return best;
    });
}

void DuplicatesController::keepShortestPath()
{
    // The copy nearest the top of the tree is usually the original; the ones
    // buried in "old", "backup" and "copy of copy" are usually not.
    //
    // Nearest the top means fewest folders deep, which is what the button says
    // and what this used to get wrong: it compared the *length* of the path, so
    // `/a/b/c/photo.jpg` beat `/documents-archive/photo.jpg` and the copy
    // somebody was promised would be kept was the one that went. Length is kept
    // as the tie-break, because two copies at the same depth have to be decided
    // somehow and the shorter name is the better guess. See MOLE-341.
    selectAllBut(
        QStringLiteral("Keeping the copy nearest the top of the tree"), [](const QList<FileEntry>& files) {
            const auto depthOf
                = [](const FileEntry& file) { return file.uri.path().count(QLatin1Char('/')); };
            int best = 0;
            for (int i = 1; i < files.size(); ++i) {
                const int here = depthOf(files.at(i));
                const int there = depthOf(files.at(best));
                if (here < there
                    || (here == there && files.at(i).uri.path().size() < files.at(best).uri.path().size())) {
                    best = i;
                }
            }
            return best;
        });
}

QString DuplicatesController::buildSetFromTicked(const QString& name)
{
    if (!m_services.isValid() || !m_services.sets)
        return {};

    // What is ticked, which is the same answer targetUris() gives -- so a set made
    // here and an operation invoked from the shell act on exactly the same files.
    const QStringList uris = selectedUris();
    if (uris.isEmpty())
        return {};

    const QString chosen = name.trimmed().isEmpty()
        ? QStringLiteral("Duplicates: %1")
              .arg(m_roots.size() == 1 ? VfsUri::fromString(m_roots.first()).fileName()
                                       : QStringLiteral("%1 folders").arg(m_roots.size()))
        : name.trimmed();

    // create() writes the file itself; the second save() here wrote the same
    // list again.
    const FileSet built = m_services.sets->create(chosen, uris);
    return built.id;
}

void DuplicatesController::deleteSelected(const QStringList& uris)
{
    if (!m_services.isValid() || m_task)
        return;

    // What the question was asked about, when the caller kept it -- and what is
    // ticked now only when it did not.
    //
    // The confirmation froze the rows it *showed* and then called this, which
    // read the ticks again at accept time. A modal does not stop the event loop:
    // a group confirmed by a scan still running, or a tick landing behind the
    // dialog, changed what was deleted without changing what had been named --
    // and the headline count was a live binding, so it changed while the names
    // below it did not. See MOLE-339.
    const QStringList ticked = uris.isEmpty() ? m_groups->selectedUris() : uris;
    if (ticked.isEmpty())
        return;

    // Grouped by drive, because a delete task belongs to one backend.
    QHash<QString, QList<VfsUri>> byDrive;
    for (const QString& uri : ticked) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (parsed.isValid())
            byDrive[parsed.scheme() + QLatin1Char('/') + parsed.authority()].append(parsed);
    }

    m_deleteFailures.clear();

    for (auto it = byDrive.constBegin(); it != byDrive.constEnd(); ++it) {
        FileSystemPtr fs = m_services.vfs->resolve(it.value().first());
        if (!fs)
            continue;

        auto* task = new DeleteTask(fs, it.value());
        connect(task, &Task::finished, this, [this, task] {
            // Only the rows that are actually gone. This used to clear the lot
            // whatever happened, so a delete a read-only drive refused left an
            // empty tab and a rescan to do -- and where the ticks spanned two
            // drives, the first task to finish wiped the second's rows before it
            // had run. What could not be deleted stays on screen, still ticked,
            // with the reason beside it. See MOLE-341.
            QStringList gone;
            for (const VfsUri& uri : task->deletedUris())
                gone.append(uri.toString());
            m_groups->removeUris(gone);

            // The failures, named. A delete that did not happen and said nothing
            // is indistinguishable from one that did.
            m_deleteFailures += task->failures();

            // The rule no longer describes the ticks: some of what it chose has
            // gone and the rest is still chosen, which is nobody's rule.
            if (!gone.isEmpty())
                m_ruleText.clear();
            emit resultsChanged();
            emit selectionChanged();
        });
        m_services.tasks->submit(task);
    }
}

QVariantMap DuplicatesController::saveState() const
{
    return { { QStringLiteral("roots"), m_roots }, { QStringLiteral("strategy"), m_strategyId },
        { QStringLiteral("minimumSize"), m_minimumSize } };
}

void DuplicatesController::restoreState(const QVariantMap& state)
{
    setStrategyId(state.value(QStringLiteral("strategy"), m_strategyId).toString());
    setMinimumSize(state.value(QStringLiteral("minimumSize"), m_minimumSize).toLongLong());
    // Results are not restored: they describe a filesystem as it was, and
    // presenting stale duplicates as current is how someone deletes the wrong
    // copy of something.
    setTargets(state.value(QStringLiteral("roots")).toStringList());
}

DuplicatesFeature::DuplicatesFeature(PluginServices services)
    : m_services(services)
{
}

QUrl DuplicatesFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/DuplicatesView.qml"));
}

FeatureController* DuplicatesFeature::createController(QObject* parent)
{
    return new DuplicatesController(m_services, parent);
}

} // namespace mole
