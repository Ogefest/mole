#pragma once

#include "core/index/IndexDatabase.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QStringList>

namespace mole {

class TaskManager;
class EventBus;

/// Everything the interface asks the index about, read in one go.
///
/// Carried by value through a queued signal, so it is registered with the
/// meta-object system in CoreMetaTypes.
struct IndexOverview
{
    QList<IndexVolume> volumes;
    /// What each volume's files were recorded as stating, by volume id. The
    /// entry under -1 is every volume's merged, which is what a search over
    /// everything indexed offers.
    QHash<qint64, QStringList> factKeys;
};

/// What the interface knows about the index, without asking it.
///
/// The index is a database on a disk, and the thread that draws the window must
/// never wait on one. It used to: eight call sites ran SQL inside property
/// getters and folder-change handlers, five of them from QML bindings that
/// cannot await anything. So the interface reads this instead, and this refreshes
/// itself from a pool thread. See
/// docs/adr/0066-the-interface-reads-the-index-from-a-snapshot.md.
///
/// A refresh is triggered by `EventBus::indexUpdated`, which every finished scan
/// and every removed volume already posts.
class IndexSummary : public QObject
{
    Q_OBJECT

public:
    IndexSummary(IndexDatabase* index, TaskManager* tasks, EventBus* events, QObject* parent = nullptr);

    /// **False until the first answer has arrived**, which is a third state and
    /// not a synonym for "nothing is indexed".
    ///
    /// A caller that collapses the two says *this folder is not indexed* about a
    /// folder that is, which is a confident false statement where the stall it
    /// replaced was visibly the application's fault. Every caller renders this
    /// state as "no answer yet" -- usually by showing nothing at all.
    bool isKnown() const { return m_known; }

    /// The volumes the last finished scan of each left behind. Empty until
    /// isKnown(), and empty also when there are none -- ask isKnown() to tell
    /// those apart.
    const QList<IndexVolume>& volumes() const { return m_overview.volumes; }

    /// The fact keys recorded for `volumeId`, or every volume's merged when -1.
    QStringList factKeys(qint64 volumeId = -1) const;

    /// Ask again. A refresh already in flight is not doubled; one asked for
    /// while another is running is coalesced into a single repeat, so a burst of
    /// events costs two reads rather than one per event.
    void refresh();

    /// How many reads have landed. Monotonic.
    ///
    /// With `isReading()` this is enough to wait for a read that *began* after a
    /// call to refresh(), which is not the same as the next one to land: a read
    /// already in flight when refresh() was called answers with the state from
    /// before it, and the coalesced repeat is the one that carries the change.
    qint64 reads() const { return m_reads; }

    /// Whether a read is in flight now.
    bool isReading() const { return m_inFlight; }

    /// Why the last read failed, or empty when the last one did not.
    ///
    /// **This is what separates the third state from a fourth.** isKnown() is
    /// false both before the first answer and after a read that could not be
    /// made, and those are not the same thing to say to somebody: the first is
    /// "not yet" and the second is "this database cannot be read". A caller that
    /// renders both as nothing tells a person their tree is not indexed for as
    /// long as the fault lasts. Cleared by the next read that lands.
    /// See MOLE-405.
    QString lastError() const { return m_lastError; }

signals:
    /// The snapshot changed, or arrived for the first time.
    void changed();

private:
    void adopt(const IndexOverview& overview);

    IndexDatabase* m_index = nullptr;
    TaskManager* m_tasks = nullptr;
    IndexOverview m_overview;
    qint64 m_reads = 0;
    bool m_known = false;
    bool m_inFlight = false;
    bool m_askedAgain = false;
    QString m_lastError;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::IndexOverview)
