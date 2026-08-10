#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/sync/SyncTask.h"

#include <QPointer>
#include <QVariantList>

namespace mole {

/// A sync between two folders, on any two drives.
///
/// Everything goes through the VFS, so this works between the local disk and an
/// archive today and between an S3 bucket and a NAS the day those backends
/// arrive -- without a line changing here.
///
/// The dry run is the default. A mirror is the one operation in this application
/// that deletes things nobody asked it to touch, and finding that out afterwards
/// is finding it out too late.
class SyncController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QString sourceUri READ sourceUri WRITE setSourceUri NOTIFY endpointsChanged)
    Q_PROPERTY(QString targetUri READ targetUri WRITE setTargetUri NOTIFY endpointsChanged)
    Q_PROPERTY(bool ready READ isReady NOTIFY endpointsChanged)

    Q_PROPERTY(QVariantList modes READ modes CONSTANT)
    Q_PROPERTY(QVariantList compareChoices READ compareChoices CONSTANT)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY optionsChanged)
    Q_PROPERTY(QString modeDescription READ modeDescription NOTIFY optionsChanged)
    Q_PROPERTY(QString compare READ compare WRITE setCompare NOTIFY optionsChanged)
    Q_PROPERTY(bool skipNewer READ skipNewer WRITE setSkipNewer NOTIFY optionsChanged)
    Q_PROPERTY(bool recursive READ recursive WRITE setRecursive NOTIFY optionsChanged)
    Q_PROPERTY(bool includeHidden READ includeHidden WRITE setIncludeHidden NOTIFY optionsChanged)
    Q_PROPERTY(QString includePatterns READ includePatterns WRITE setIncludePatterns NOTIFY optionsChanged)
    Q_PROPERTY(QString excludePatterns READ excludePatterns WRITE setExcludePatterns NOTIFY optionsChanged)

    Q_PROPERTY(QVariantList steps READ steps NOTIFY planChanged)
    Q_PROPERTY(QString planSummary READ planSummary NOTIFY planChanged)
    Q_PROPERTY(bool hasPlan READ hasPlan NOTIFY planChanged)
    Q_PROPERTY(int deleteCount READ deleteCount NOTIFY planChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY planChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY planChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY planChanged)

public:
    SyncController(PluginServices services, QObject* parent = nullptr);
    ~SyncController() override;

    QString sourceUri() const { return m_source; }
    void setSourceUri(const QString& uri);
    QString targetUri() const { return m_target; }
    void setTargetUri(const QString& uri);
    bool isReady() const;
    /// The shell hands the tab whatever the previous one was aimed at; the first
    /// folder becomes the source, which is what opening it from a folder means.
    Q_INVOKABLE void setTargets(const QStringList& uris);
    Q_INVOKABLE void swapEnds();

    QVariantList modes() const;
    QVariantList compareChoices() const;
    QString mode() const;
    void setMode(const QString& mode);
    QString modeDescription() const;
    QString compare() const;
    void setCompare(const QString& compare);
    bool skipNewer() const { return m_options.skipNewer; }
    void setSkipNewer(bool skip);
    bool recursive() const { return m_options.recursive; }
    void setRecursive(bool recursive);
    bool includeHidden() const { return m_options.includeHidden; }
    void setIncludeHidden(bool include);
    QString includePatterns() const { return m_options.includePatterns.join(QLatin1Char(';')); }
    void setIncludePatterns(const QString& patterns);
    QString excludePatterns() const { return m_options.excludePatterns.join(QLatin1Char(';')); }
    void setExcludePatterns(const QString& patterns);

    QVariantList steps() const;
    QString planSummary() const;
    bool hasPlan() const { return m_hasPlan; }
    int deleteCount() const;
    bool isRunning() const { return !m_task.isNull(); }
    QString progressText() const { return m_progressText; }
    QString errorText() const { return m_errorText; }

    /// Works out what would happen and stops.
    /// The deletions in the plan, listed for the confirmation. Only the deletions:
    /// a plan can be thousands of steps, and the one being agreed to here is the
    /// part that destroys something.
    Q_INVOKABLE QVariantList deletions() const;

    Q_INVOKABLE void preview();
    /// Does it. Deliberately a separate act from previewing.
    Q_INVOKABLE void apply();
    Q_INVOKABLE void cancel();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void endpointsChanged();
    void optionsChanged();
    void planChanged();

private:
    void start(bool dryRun);

    PluginServices m_services;
    QString m_source;
    QString m_target;
    SyncOptions m_options;
    SyncPlan m_plan;
    bool m_hasPlan = false;
    bool m_lastWasDryRun = true;
    QString m_progressText;
    QString m_errorText;
    QPointer<SyncTask> m_task;
};

class SyncFeature final : public IFeature
{
public:
    explicit SyncFeature(PluginServices services);

    QString id() const override { return QStringLiteral("core.sync"); }
    QString title() const override { return QStringLiteral("Sync"); }
    QString description() const override
    {
        return QStringLiteral("Make one folder resemble another, on any two drives");
    }
    QString iconText() const override { return QStringLiteral("⇉"); }
    int sortOrder() const override { return 44; }
    /// Needs two endpoints. Opened from nothing it has neither.
    bool needsContext() const override { return true; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
