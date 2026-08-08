#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/duplicates/FindDuplicatesTask.h"

#include <QPointer>
#include <QVariantList>

namespace mole {

/// Finding duplicates, and deciding what to do about them.
///
/// Choosing what to keep is the hard half. The tab never picks for the user, but
/// it offers the choices people actually make -- keep the oldest, keep the one
/// in a particular folder -- and shows what each would free before anything is
/// deleted.
class DuplicatesController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QStringList roots READ roots NOTIFY rootsChanged)
    Q_PROPERTY(QVariantList strategies READ strategies CONSTANT)
    Q_PROPERTY(QString strategyId READ strategyId WRITE setStrategyId NOTIFY optionsChanged)
    Q_PROPERTY(QString strategyDescription READ strategyDescription NOTIFY optionsChanged)
    Q_PROPERTY(qint64 minimumSize READ minimumSize WRITE setMinimumSize NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY resultsChanged)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY resultsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY resultsChanged)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY resultsChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY resultsChanged)
    /// Files the user has ticked for removal, and what removing them frees.
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedSizeText READ selectedSizeText NOTIFY selectionChanged)

public:
    DuplicatesController(PluginServices services, QObject* parent = nullptr);
    ~DuplicatesController() override;

    QStringList roots() const { return m_roots; }
    Q_INVOKABLE void setTargets(const QStringList& uris);

    QVariantList strategies() const;
    QString strategyId() const { return m_strategyId; }
    void setStrategyId(const QString& id);
    QString strategyDescription() const;
    qint64 minimumSize() const { return m_minimumSize; }
    void setMinimumSize(qint64 bytes);

    QVariantList groups() const;
    int groupCount() const { return static_cast<int>(m_groups.size()); }
    QString summary() const;
    bool isScanning() const { return !m_task.isNull(); }
    bool hasRun() const { return m_hasRun; }

    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    QString selectedSizeText() const;
    Q_INVOKABLE QStringList selectedUris() const;
    /// The ticked copies, listed for the confirmation: full location rather than
    /// name, because in a duplicate group every name is the same and the location
    /// is the only thing that tells one from another.
    Q_INVOKABLE QVariantList selectedDetails() const;
    /// The same name every other operation uses, so a chosen pile of duplicates
    /// can be added to a set, renamed or reported on without this tab knowing
    /// how any of that works.
    Q_INVOKABLE QStringList targetUris() const { return selectedUris(); }

    Q_INVOKABLE void scan();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void toggle(const QString& uri);
    Q_INVOKABLE void clearSelection();
    /// Ticks everything in each group except the one the rule keeps. The tab
    /// never picks for the user without being asked -- these are the asking.
    Q_INVOKABLE void keepNewest();
    Q_INVOKABLE void keepOldest();
    Q_INVOKABLE void keepShortestPath();
    Q_INVOKABLE void deleteSelected();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void rootsChanged();
    void optionsChanged();
    void resultsChanged();
    void selectionChanged();

private:
    void selectAllBut(const std::function<int(const QList<FileEntry>&)>& chooseKeeper);

    PluginServices m_services;
    QStringList m_roots;
    QString m_strategyId = QStringLiteral("content");
    qint64 m_minimumSize = 1024;
    QList<DuplicateGroup> m_groups;
    QSet<QString> m_selected;
    bool m_hasRun = false;
    QPointer<Task> m_task;
};

class DuplicatesFeature final : public IFeature
{
public:
    explicit DuplicatesFeature(PluginServices services);

    QString id() const override { return QStringLiteral("core.duplicates"); }
    QString title() const override { return QStringLiteral("Duplicates"); }
    QString description() const override
    {
        return QStringLiteral("Find files that are the same, several ways");
    }
    QString iconText() const override { return QStringLiteral("⧉"); }
    int sortOrder() const override { return 46; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
