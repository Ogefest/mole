#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/automation/ScheduleRule.h"
#include "core/index/IndexDatabase.h"

#include <QVariantList>

namespace mole {

/// Every index that exists, in one place.
///
/// An index is a claim about a tree that goes quietly out of date, and until
/// this tab existed the only place an indexed volume appeared in the whole
/// interface was a dropdown inside the search form, as "<label> (<n> entries)".
/// Not the tree it covers, not when it was last scanned, not what kind of scan
/// produced it, not whether anything is keeping it fresh -- so the freshness of
/// every search answered from an index was decided by something nobody could
/// look at.
///
/// The same argument as *Reports*, which exists because a series is only useful
/// if you can find it.
class IndexesController final : public FeatureController
{
    Q_OBJECT
    /// One entry per indexed volume, stalest first: the one worth looking at is
    /// the one that has not been scanned for longest.
    Q_PROPERTY(QVariantList volumes READ volumes NOTIFY volumesChanged)
    Q_PROPERTY(int volumeCount READ volumeCount NOTIFY volumesChanged)
    /// Entries across every index, which is what an index costs in one number.
    Q_PROPERTY(QString totalEntriesText READ totalEntriesText NOTIFY volumesChanged)
    /// How many are kept fresh by a schedule. The rest are as old as their last
    /// scan and no newer.
    Q_PROPERTY(int scheduledCount READ scheduledCount NOTIFY volumesChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    explicit IndexesController(PluginServices services, QObject* parent = nullptr);

    QVariantList volumes() const;
    int volumeCount() const;
    QString totalEntriesText() const;
    int scheduledCount() const;

    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);

    /// Re-reads the index and the schedule store. Called when a scan finishes
    /// elsewhere, so the list is never older than the tab.
    Q_INVOKABLE void refresh();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void volumesChanged();
    void filterChanged();

private:
    void rebuild();
    /// The rule keeping `rootUri` fresh, if there is one. A volume's rule is the
    /// index job whose root matches, which is what the search form already looks
    /// for one uri at a time.
    std::optional<ScheduleRule> ruleFor(const QString& rootUri) const;

    PluginServices m_services;
    QList<IndexVolume> m_volumes;
    QString m_filter;
};

class IndexesFeature final : public IFeature
{
public:
    explicit IndexesFeature(PluginServices services);

    QString id() const override { return QStringLiteral("core.indexes"); }
    QString title() const override { return QStringLiteral("Indexes"); }
    QString description() const override
    {
        return QStringLiteral("Every indexed tree: how old it is and what is in it");
    }
    QString iconText() const override { return QStringLiteral("⛁"); }
    /// Beside Reports, which is the same idea for the other thing Mole keeps.
    int sortOrder() const override { return 43; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
