#pragma once

#include "core/automation/Chain.h"
#include "core/data/JsonFileStore.h"

#include <QList>
#include <QObject>

namespace mole {

/// Where the chains somebody built live.
///
/// One file, written whole and atomically, beside ScheduleStore and
/// FileSetStore: the list is small, and a torn write that lost it would take
/// every scheduled chain with it.
///
/// **A chain that will not load is dropped and said to have been dropped.** The
/// alternative is refusing the whole file, which loses nine good chains to one bad
/// one -- but a chain that silently disappears is a scheduled job that stops
/// happening with nothing to read about it, so the count is available to whoever
/// wants to say so.
class ChainStore : public JsonFileStore
{
    Q_OBJECT

public:
    explicit ChainStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_CHAINS_PATH, so a test and the harness never touch the real
    /// one.
    static QString defaultPath();

    bool load();
    [[nodiscard]] bool save();

    [[nodiscard]] QList<Chain> chains() const { return m_chains; }
    [[nodiscard]] Chain chain(const QString& id) const;
    /// Adds or replaces by id, keeping the order things were made in. False for a
    /// chain with no id.
    bool put(const Chain& chain);
    bool remove(const QString& id);

    /// How many chains the last load could not read. Zero after a clean load, and
    /// the reason this is not silent.
    [[nodiscard]] int unreadable() const { return m_unreadable; }

signals:
    void chainsChanged();

private:
    QList<Chain> m_chains;
    int m_unreadable = 0;
};

} // namespace mole
