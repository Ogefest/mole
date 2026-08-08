#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/IPreviewProvider.h"

#include "core/vfs/FileEntry.h"

#include <QPointer>
#include <QUrl>

namespace mole {

class ListDirectoryTask;

/// A tab that shows one file at a time.
///
/// Named for the tab, not the viewer: the SDK's PreviewController is the base
/// an individual viewer subclasses, and this owns one of those at a time.
///
/// It owns two things the individual viewers do not: which file is being shown,
/// and the list of its neighbours so the arrow keys can step through the folder
/// without going back to the browser. Everything about *how* a file looks comes
/// from an IPreviewProvider, so a plugin adding a viewer needs to touch nothing
/// here.
class PreviewTabController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY currentChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY currentChanged)
    Q_PROPERTY(QString viewerName READ viewerName NOTIFY currentChanged)
    /// QML component for the current file, empty when nothing is open.
    Q_PROPERTY(QUrl viewSource READ viewSource NOTIFY currentChanged)
    /// The object that component binds to. Owned by this controller.
    Q_PROPERTY(QObject* viewer READ viewer NOTIFY currentChanged)
    Q_PROPERTY(int position READ position NOTIFY currentChanged)
    Q_PROPERTY(int siblingCount READ siblingCount NOTIFY currentChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY currentChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY currentChanged)

public:
    explicit PreviewTabController(PluginServices services, QObject* parent = nullptr);
    ~PreviewTabController() override;

    QString currentUri() const { return m_current.uri.toString(); }
    QString fileName() const { return m_current.name; }
    QString folderPath() const;
    QString viewerName() const { return m_viewerName; }
    QUrl viewSource() const { return m_viewSource; }
    QObject* viewer() const;
    /// One-based, for "3 of 17".
    int position() const;
    int siblingCount() const { return static_cast<int>(m_siblings.size()); }
    bool canGoNext() const { return position() > 0 && position() < siblingCount(); }
    bool canGoPrevious() const { return position() > 1; }

    /// Shows `uri` and loads its folder in the background so the arrows work.
    Q_INVOKABLE void open(const QString& uri);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void currentChanged();

private:
    void showEntry(const FileEntry& entry);
    void loadSiblings(const VfsUri& directory, const VfsUri& select);
    void step(int delta);

    PluginServices m_services;
    FileEntry m_current;
    /// Files only: stepping into a directory from a preview makes no sense.
    FileEntryList m_siblings;
    QString m_viewerName;
    QUrl m_viewSource;
    QPointer<QObject> m_viewer;
    QPointer<ListDirectoryTask> m_listing;
};

class PreviewFeature final : public IFeature
{
public:
    explicit PreviewFeature(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview"); }
    QString title() const override { return QStringLiteral("Preview"); }
    QString description() const override
    {
        return QStringLiteral("Look inside one file, and step through the folder with the arrows.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F441"); }
    bool needsContext() const override { return true; }
    int sortOrder() const override { return 25; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
