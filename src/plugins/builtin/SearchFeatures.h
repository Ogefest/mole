#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "ui/models/FileListModel.h"

#include "core/index/IndexDatabase.h"
#include "core/search/LiveSearchTask.h"

#include <QPointer>
#include <QStringList>

#include <optional>

namespace mole {

class IndexSearchTask;

/// Walks the filesystem now. Always current, costs a traversal.
class LiveSearchController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(mole::FileListModel* results READ results CONSTANT)
    Q_PROPERTY(QString rootUri READ rootUri WRITE setRootUri NOTIFY rootUriChanged)
    Q_PROPERTY(QString queryText READ queryText WRITE setQueryText NOTIFY queryTextChanged)
    Q_PROPERTY(QString extension READ extension WRITE setExtension NOTIFY criteriaChanged)
    Q_PROPERTY(bool caseSensitive READ caseSensitive WRITE setCaseSensitive NOTIFY criteriaChanged)
    /// Bytes; -1 for "no limit". Set through setSizeRange() from the form, which
    /// takes what a person types.
    Q_PROPERTY(qint64 minSize READ minSize NOTIFY criteriaChanged)
    Q_PROPERTY(qint64 maxSize READ maxSize NOTIFY criteriaChanged)
    /// Answer from the index when it covers this folder. On by default: the index
    /// is enormously faster and, for a folder that was indexed, usually right.
    /// See docs/adr/0005-which-engine-answers-a-search.md.
    Q_PROPERTY(bool useIndex READ useIndex WRITE setUseIndex NOTIFY criteriaChanged)
    /// True when an indexed volume's root is a prefix of this folder, so the index
    /// covers the whole subtree. Partial coverage counts as none.
    Q_PROPERTY(bool indexCoversRoot READ indexCoversRoot NOTIFY rootUriChanged)
    /// "last scanned 2 hours ago", for the form to show beside the toggle. Empty
    /// when nothing covers this folder.
    Q_PROPERTY(QString indexNote READ indexNote NOTIFY rootUriChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool truncated READ isTruncated NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)

public:
    LiveSearchController(PluginServices services, QString rootUri, QObject* parent = nullptr);
    ~LiveSearchController() override;

    FileListModel* results() const { return m_results; }
    QString rootUri() const { return m_rootUri; }
    void setRootUri(const QString& uri);
    QString queryText() const { return m_queryText; }
    void setQueryText(const QString& text);
    QString extension() const { return m_extension; }
    void setExtension(const QString& extension);
    bool caseSensitive() const { return m_caseSensitive; }
    void setCaseSensitive(bool sensitive);
    bool isRunning() const { return m_running; }
    bool isTruncated() const { return m_truncated; }
    QString statusText() const { return m_statusText; }

    qint64 minSize() const { return m_minSize; }
    qint64 maxSize() const { return m_maxSize; }
    bool useIndex() const { return m_useIndex; }
    void setUseIndex(bool use);
    bool indexCoversRoot() const;
    QString indexNote() const;

    /// Takes what a person types -- "10M", "1.5 GB", "500k", or nothing at all --
    /// and returns the bytes, or -1 for anything it cannot make sense of. A form
    /// should not make someone count zeros.
    static qint64 parseSize(const QString& text);
    Q_INVOKABLE void setSizeRange(const QString& minText, const QString& maxText);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void rootUriChanged();
    void queryTextChanged();
    void criteriaChanged();
    void runningChanged();
    void statusChanged();

private:
    /// The deepest indexed volume whose root is a prefix of the search root, if
    /// any covers it at all.
    std::optional<IndexVolume> coveringVolume() const;
    void setRunning(bool running);
    void setStatusText(const QString& text);

    PluginServices m_services;
    FileListModel* m_results = nullptr;
    QString m_rootUri;
    QString m_queryText;
    QString m_extension;
    bool m_caseSensitive = false;
    qint64 m_minSize = -1;
    qint64 m_maxSize = -1;
    bool m_useIndex = true;
    bool m_running = false;
    bool m_truncated = false;
    QString m_statusText;
    QPointer<LiveSearchTask> m_task;
    /// The other engine. Only ever one of the two is running.
    QPointer<IndexSearchTask> m_indexTask;
};

/// Queries what a previous scan recorded. Instant, possibly stale.
class IndexSearchController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(mole::FileListModel* results READ results CONSTANT)
    Q_PROPERTY(QString queryText READ queryText WRITE setQueryText NOTIFY queryTextChanged)
    Q_PROPERTY(QStringList volumeLabels READ volumeLabels NOTIFY volumesChanged)
    Q_PROPERTY(int volumeIndex READ volumeIndex WRITE setVolumeIndex NOTIFY volumeIndexChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    explicit IndexSearchController(PluginServices services, QObject* parent = nullptr);
    ~IndexSearchController() override;

    FileListModel* results() const { return m_results; }
    QString queryText() const { return m_queryText; }
    void setQueryText(const QString& text);
    /// "All volumes" followed by each scanned root.
    QStringList volumeLabels() const { return m_volumeLabels; }
    int volumeIndex() const { return m_volumeIndex; }
    void setVolumeIndex(int index);
    QString statusText() const { return m_statusText; }
    bool isRunning() const { return m_running; }

    Q_INVOKABLE void search();
    Q_INVOKABLE void refreshVolumes();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;
    /// Queues a background scan that fills the index for `uri`.
    Q_INVOKABLE void scanDirectory(const QString& uri, const QString& label);

signals:
    void queryTextChanged();
    void volumesChanged();
    void volumeIndexChanged();
    void statusChanged();
    void runningChanged();

private:
    void setStatusText(const QString& text);
    void setRunning(bool running);

    PluginServices m_services;
    FileListModel* m_results = nullptr;
    QString m_queryText;
    QStringList m_volumeLabels;
    QList<qint64> m_volumeIds;
    int m_volumeIndex = 0;
    QString m_statusText;
    bool m_running = false;
    QPointer<IndexSearchTask> m_task;
};

class LiveSearchFeature final : public IFeature
{
public:
    LiveSearchFeature(PluginServices services, QString defaultRoot);

    QString id() const override { return QStringLiteral("mole.livesearch"); }
    QString title() const override { return QStringLiteral("Quick search"); }
    QString description() const override
    {
        return QStringLiteral("Walk a tree and find files by name, right now.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F50D"); }
    int sortOrder() const override { return 20; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    QString m_defaultRoot;
};

class IndexSearchFeature final : public IFeature
{
public:
    explicit IndexSearchFeature(PluginServices services);

    QString id() const override { return QStringLiteral("mole.indexsearch"); }
    QString title() const override { return QStringLiteral("Indexed search"); }
    QString description() const override
    {
        return QStringLiteral("Search volumes you scanned earlier, without touching the disk.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F5C2"); }
    int sortOrder() const override { return 30; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
