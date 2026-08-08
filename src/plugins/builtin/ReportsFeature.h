#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/analysis/AnalysisStore.h"

#include <QVariantList>

namespace mole {

class AnalysisStore;

/// Every report that has been produced and kept, in one place.
///
/// Reports are worth far more as a series than as a snapshot, but a series is
/// only useful if you can find it. Without this the only route to a saved
/// report was to navigate back to the folder it came from and remember that a
/// report existed at all.
class ReportsController final : public FeatureController
{
    Q_OBJECT
    /// One entry per analysed folder, newest activity first.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    /// Every run of the folder currently selected.
    Q_PROPERTY(QVariantList runs READ runs NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedRoot READ selectedRoot WRITE setSelectedRoot NOTIFY selectionChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int folderCount READ folderCount NOTIFY foldersChanged)
    Q_PROPERTY(int reportCount READ reportCount NOTIFY foldersChanged)
    Q_PROPERTY(QString totalSizeText READ totalSizeText NOTIFY foldersChanged)

public:
    ReportsController(PluginServices services, AnalysisStore* store, QObject* parent = nullptr);

    QVariantList folders() const;
    QVariantList runs() const;

    QString selectedRoot() const { return m_selectedRoot; }
    void setSelectedRoot(const QString& root);
    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);

    int folderCount() const;
    int reportCount() const;
    /// What the reported trees add up to, from each folder's newest run.
    QString totalSizeText() const;

    /// Re-reads the store. Called after a report is filed elsewhere.
    Q_INVOKABLE void refresh();
    /// Drops one run, or every run of a folder when the id is empty.
    Q_INVOKABLE bool removeRun(const QString& rootUri, const QString& id);
    Q_INVOKABLE bool forgetFolder(const QString& rootUri);

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void foldersChanged();
    void selectionChanged();
    void filterChanged();

private:
    void rebuild();

    PluginServices m_services;
    AnalysisStore* m_store = nullptr;
    QString m_selectedRoot;
    QString m_filter;

    struct Folder
    {
        QString rootUri;
        QString label;
        QList<ReportSummary> runs;
    };
    QList<Folder> m_folders;
};

class ReportsFeature final : public IFeature
{
public:
    ReportsFeature(PluginServices services, AnalysisStore* store);

    QString id() const override { return QStringLiteral("core.reports"); }
    QString title() const override { return QStringLiteral("Reports"); }
    QString description() const override
    {
        return QStringLiteral("Every saved directory report, ready to open");
    }
    QString iconText() const override { return QStringLiteral("\U0001F5C2"); }
    int sortOrder() const override { return 42; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    AnalysisStore* m_store = nullptr;
};

} // namespace mole
