#include "core/index/IndexSummary.h"

#include "core/events/EventBus.h"
#include "core/tasks/Task.h"
#include "core/tasks/TaskManager.h"

#include <utility>

namespace mole {

/// Reads the whole overview on a pool thread.
///
/// Background work and one of many, so refreshing what the interface knows does
/// not scroll somebody's real copies and scans off the task strip.
class ReadIndexSummaryTask final : public Task
{
    Q_OBJECT

public:
    explicit ReadIndexSummaryTask(IndexDatabase* index)
        : Task(QStringLiteral("Read what is indexed"))
        , m_index(index)
    {
        setBackground(true);
        setOneOfMany(true);
    }

signals:
    /// Emitted on the UI thread. Carries the answer by value, so nothing here
    /// has to outlive the delivery.
    void ready(mole::IndexOverview overview);

protected:
    void run() override
    {
        if (!m_index || !m_index->isOpen())
            return;

        const Result<QList<IndexVolume>> volumes = m_index->volumes();
        if (!volumes.ok()) {
            // Failed rather than finished quietly. This used to set a status
            // line on a background, one-of-many task -- which is a line nobody
            // is shown -- and return, leaving the snapshot's isKnown() false for
            // ever: every caller renders that as "no answer yet", so a database
            // that cannot be read looked exactly like one that had not answered
            // yet, permanently. See MOLE-405.
            fail(volumes.error());
            return;
        }

        IndexOverview overview;
        overview.volumes = volumes.value();

        // Every volume's keys and the merged answer, in one pass. One query per
        // volume per refresh, against one per volume per binding evaluation
        // before -- which is what made this worth moving rather than caching in
        // place.
        if (const Result<QStringList> everywhere = m_index->factKeys(-1); everywhere.ok())
            overview.factKeys.insert(-1, everywhere.value());
        for (const IndexVolume& volume : overview.volumes) {
            if (const Result<QStringList> mine = m_index->factKeys(volume.id); mine.ok())
                overview.factKeys.insert(volume.id, mine.value());
        }

        setStatusText(
            QStringLiteral("%1 indexed %2")
                .arg(overview.volumes.size())
                .arg(overview.volumes.size() == 1 ? QStringLiteral("volume") : QStringLiteral("volumes")));
        emit ready(overview);
    }

private:
    IndexDatabase* m_index = nullptr;
};

IndexSummary::IndexSummary(IndexDatabase* index, TaskManager* tasks, EventBus* events, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_tasks(tasks)
{
    // Every finished scan and every removed volume already posts this, and
    // SearchFeatures and IndexesFeature already re-read on it. This replaces
    // those reads rather than adding a channel beside them.
    if (events)
        connect(events, &EventBus::indexUpdated, this, [this](qint64, qint64) { refresh(); });
}

QStringList IndexSummary::factKeys(qint64 volumeId) const
{
    return m_overview.factKeys.value(volumeId);
}

void IndexSummary::refresh()
{
    if (!m_index || !m_tasks)
        return;
    if (m_inFlight) {
        // Coalesced rather than queued: four scans finishing together is one
        // repeat, not four reads of the same small table.
        m_askedAgain = true;
        return;
    }

    m_inFlight = true;
    auto* task = new ReadIndexSummaryTask(m_index);
    connect(task, &ReadIndexSummaryTask::ready, this, &IndexSummary::adopt);
    // Whether it answered or not, the flight is over -- a failed read must not
    // leave this permanently unable to try again.
    connect(task, &Task::finished, this, [this, task] {
        m_inFlight = false;
        // A read that failed is remembered, and said out loud: isKnown() stays
        // false because nothing *is* known, so without this the third state and
        // "the database is broken" are the same state to every caller.
        if (task->state() == Task::State::Failed) {
            m_lastError = task->error().message;
            emit changed();
        }
        if (std::exchange(m_askedAgain, false))
            refresh();
    });
    m_tasks->submit(task);
}

void IndexSummary::adopt(const IndexOverview& overview)
{
    m_overview = overview;
    m_known = true;
    m_lastError.clear();
    ++m_reads;
    emit changed();
}

} // namespace mole

#include "IndexSummary.moc"
