#pragma once

#include "ui/SessionStore.h"

#include <QAbstractListModel>
#include <QList>
#include <QUrl>

namespace mole {

class FeatureRegistry;
class FeatureController;

/// The open tabs.
///
/// A tab is a feature id plus the controller instance holding that tab's
/// state. The shell renders whatever QML the feature points at, so it never
/// needs to know a browser from a duplicate finder.
class TabsModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    enum Role {
        FeatureIdRole = Qt::UserRole + 1,
        TitleRole,
        SubtitleRole,
        IconTextRole,
        ViewSourceRole,
        ControllerRole,
        BusyRole,
    };

    explicit TabsModel(FeatureRegistry* registry, QObject* parent = nullptr);
    ~TabsModel() override;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int index);

    /// Opens a tab for `featureId` and makes it current. Returns the new row,
    /// or -1 when the feature is not registered.
    Q_INVOKABLE int openTab(const QString& featureId);
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void closeCurrentTab();

    /// The controller of a given tab, for QML that needs to talk to it
    /// directly (e.g. the sidebar telling the current browser to navigate).
    Q_INVOKABLE QObject* controllerAt(int index) const;
    Q_INVOKABLE QObject* currentController() const;

    /// Asks every open tab what it wants remembered.
    Session captureSession() const;
    /// Re-creates the tabs. Entries whose feature is no longer registered --
    /// an uninstalled plugin -- are skipped rather than failing the restore.
    /// Returns how many came back.
    int restoreSession(const Session& session);

signals:
    void countChanged();
    void currentIndexChanged();
    void tabOpened(int index);
    /// Something worth persisting changed: a tab opened, closed, was selected,
    /// or reported its own state moved on.
    void sessionDirty();

private:
    struct Tab
    {
        QString featureId;
        QString iconText;
        QUrl viewSource;
        FeatureController* controller = nullptr;
        /// Identity that survives rows shifting as neighbours close.
        int id = 0;
        /// The tab this one was opened from, so closing it can hand the user
        /// back where they came from. -1 when it was opened from nowhere.
        int openedFromId = -1;
    };

    void emitRowChanged(const FeatureController* controller, const QList<int>& roles);
    int rowOfTabId(int id) const;
    /// Selects a row and always notifies, even when the number is unchanged:
    /// after a removal the same index is a different tab.
    void selectRow(int index);

    FeatureRegistry* m_registry = nullptr;
    QList<Tab> m_tabs;
    int m_currentIndex = -1;
    int m_nextTabId = 1;
};

} // namespace mole
