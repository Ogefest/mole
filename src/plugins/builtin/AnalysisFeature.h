#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "ui/models/BreakdownModel.h"

#include "core/analysis/AnalyseDirectoryTask.h"
#include "core/analysis/AnalysisStore.h"
#include "core/analysis/ReportDiff.h"

#include <QPointer>
#include <QVariantList>

#include <memory>

namespace mole {

/// One directory under analysis: its latest report, its history, and the
/// comparison the user picked.
///
/// A separate object per directory because selecting two folders means two
/// independent reports and two independent histories -- merging them would
/// answer a question nobody asked.
class AnalysisTarget : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString rootUri READ rootUri CONSTANT)
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(bool busy READ isBusy NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool hasReport READ hasReport NOTIFY reportChanged)

    /// Headline numbers, ready to drop into tiles.
    Q_PROPERTY(QVariantMap headline READ headline NOTIFY reportChanged)
    Q_PROPERTY(mole::BreakdownModel* extensions READ extensions CONSTANT)
    Q_PROPERTY(QVariantList topFolders READ topFolders NOTIFY reportChanged)
    Q_PROPERTY(QVariantList largestFiles READ largestFiles NOTIFY reportChanged)
    Q_PROPERTY(QVariantList sizeBuckets READ sizeBuckets NOTIFY reportChanged)
    Q_PROPERTY(QVariantList ageBuckets READ ageBuckets NOTIFY reportChanged)

    /// Past runs, newest first, for the comparison picker.
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QString comparisonId READ comparisonId WRITE setComparisonId NOTIFY diffChanged)
    Q_PROPERTY(bool hasDiff READ hasDiff NOTIFY diffChanged)
    /// "" when this folder is not on a schedule, else "Every day" and friends.
    Q_PROPERTY(QString scheduleText READ scheduleText NOTIFY scheduleChanged)
    Q_PROPERTY(qint64 scheduleSeconds READ scheduleSeconds NOTIFY scheduleChanged)
    Q_PROPERTY(QVariantList schedulePresets READ schedulePresets CONSTANT)
    Q_PROPERTY(QVariantMap diffHeadline READ diffHeadline NOTIFY diffChanged)
    Q_PROPERTY(QVariantList diffRows READ diffRows NOTIFY diffChanged)

public:
    AnalysisTarget(PluginServices services, AnalysisStore* store, QString rootUri, QString label,
        QObject* parent = nullptr);
    ~AnalysisTarget() override;

    QString rootUri() const { return m_rootUri; }
    QString label() const { return m_label; }
    bool isBusy() const { return m_busy; }
    QString statusText() const { return m_statusText; }
    bool hasReport() const { return m_report.isValid(); }

    QString scheduleText() const;
    qint64 scheduleSeconds() const;
    QVariantList schedulePresets() const;
    /// Puts this folder on the clock. `seconds` of 0 takes it off again.
    Q_INVOKABLE void setSchedule(qint64 seconds);

    QVariantMap headline() const;
    BreakdownModel* extensions() const { return m_extensions; }
    QVariantList topFolders() const;
    QVariantList largestFiles() const;
    QVariantList sizeBuckets() const;
    QVariantList ageBuckets() const;

    QVariantList history() const;
    QString comparisonId() const { return m_comparisonId; }
    void setComparisonId(const QString& id);
    bool hasDiff() const { return m_diff.valid; }
    QVariantMap diffHeadline() const;
    QVariantList diffRows() const;

    /// Walks the directory again and files the result in the history.
    Q_INVOKABLE void refresh();
    /// Shows a stored report instead of the newest one.
    Q_INVOKABLE void showReport(const QString& id);

    /// Loads the newest stored report, if any, without walking anything.
    void loadLatest();

signals:
    void scheduleChanged();
    void stateChanged();
    void reportChanged();
    void historyChanged();
    void diffChanged();

private:
    void setReport(AnalysisReport report);
    void recomputeDiff();

    PluginServices m_services;
    AnalysisStore* m_store = nullptr;
    QString m_rootUri;
    QString m_label;
    AnalysisReport m_report;
    ReportDiff m_diff;
    QString m_comparisonId;
    BreakdownModel* m_extensions = nullptr;
    bool m_busy = false;
    QString m_statusText;
    QPointer<AnalyseDirectoryTask> m_task;
};

/// The analysis tab. Holds one target per directory the user selected.
class AnalysisTabController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QVariantList targets READ targetList NOTIFY targetsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    Q_PROPERTY(mole::AnalysisTarget* current READ current NOTIFY currentChanged)
    Q_PROPERTY(int targetCount READ targetCount NOTIFY targetsChanged)

public:
    AnalysisTabController(PluginServices services, AnalysisStore* store, QObject* parent = nullptr);

    /// Adds a directory, or selects it if it is already here.
    Q_INVOKABLE void addTarget(const QString& rootUri, const QString& label = {});
    /// Replaces every target with these, then analyses them all.
    /// Shows these folders, loading each one's saved report. Only a folder with
    /// nothing saved is walked -- opening a report must not cost a rescan.
    Q_INVOKABLE void setTargets(const QStringList& rootUris);
    /// The same, then walks all of them. This is what "Analyse folder" means.
    Q_INVOKABLE void analyse(const QStringList& rootUris);
    Q_INVOKABLE void refreshAll();

    QVariantList targetList() const;
    int targetCount() const { return static_cast<int>(m_targets.size()); }
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int index);
    AnalysisTarget* current() const;

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void targetsChanged();
    void currentChanged();

private:
    void refreshTitle();

    PluginServices m_services;
    AnalysisStore* m_store = nullptr;
    QList<AnalysisTarget*> m_targets;
    int m_currentIndex = -1;
};

class AnalysisFeature final : public IFeature
{
public:
    explicit AnalysisFeature(PluginServices services);
    ~AnalysisFeature() override;

    QString id() const override { return QStringLiteral("mole.analysis"); }
    QString title() const override { return QStringLiteral("Analysis"); }
    QString description() const override
    {
        return QStringLiteral("What a folder is made of, and how it changed since last time.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F4CA"); }
    /// A report is a report *of* somewhere. With nothing open there is no
    /// folder to analyse, so offering it would open an empty tab.
    bool needsContext() const override { return true; }
    int sortOrder() const override { return 40; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

    /// Shared with the scheduled job so a run started by the clock lands in
    /// the same history the user compares against by hand.
    AnalysisStore* store() const { return m_store; }

private:
    PluginServices m_services;
    /// Owned by the host, shared by every analysis tab, so histories do not
    /// depend on which tab happened to write them.
    AnalysisStore* m_store = nullptr;
};

} // namespace mole
