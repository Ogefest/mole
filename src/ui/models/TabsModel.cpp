#include "ui/models/TabsModel.h"

#include "host/FeatureRegistry.h"
#include "sdk/FeatureController.h"

namespace mole {

TabsModel::TabsModel(FeatureRegistry* registry, QObject* parent)
    : QAbstractListModel(parent)
    , m_registry(registry)
{
}

TabsModel::~TabsModel() = default;

int TabsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tabs.size());
}

QVariant TabsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tabs.size())
        return {};

    const Tab& tab = m_tabs.at(index.row());
    switch (role) {
    case FeatureIdRole:
        return tab.featureId;
    case TitleRole:
    case Qt::DisplayRole:
        return tab.controller ? tab.controller->title() : QString();
    case SubtitleRole:
        return tab.controller ? tab.controller->subtitle() : QString();
    case IconTextRole:
        return tab.iconText;
    case ViewSourceRole:
        return tab.viewSource;
    case ControllerRole:
        return QVariant::fromValue(static_cast<QObject*>(tab.controller));
    case BusyRole:
        return tab.controller && tab.controller->isBusy();
    case OpenerTitleRole: {
        const int opener = rowOfTabId(tab.openedFromId);
        if (opener < 0)
            return QString();
        return m_tabs.at(opener).controller ? m_tabs.at(opener).controller->title() : QString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> TabsModel::roleNames() const
{
    return {
        { FeatureIdRole, "featureId" },
        { TitleRole, "title" },
        { SubtitleRole, "subtitle" },
        { IconTextRole, "iconText" },
        { ViewSourceRole, "viewSource" },
        { ControllerRole, "controller" },
        { BusyRole, "busy" },
        { OpenerTitleRole, "openerTitle" },
    };
}

int TabsModel::openTab(const QString& featureId)
{
    if (!m_registry)
        return -1;
    IFeature* feature = m_registry->feature(featureId);
    if (!feature)
        return -1;

    Tab tab;
    tab.featureId = featureId;
    tab.iconText = feature->iconText();
    tab.viewSource = feature->viewSource();
    tab.controller = feature->createController(this);
    if (!tab.controller)
        return -1;

    tab.id = m_nextTabId++;
    // Recorded before the insert, while m_currentIndex still points at the tab
    // the user opened this one from.
    tab.openedFromId
        = m_currentIndex >= 0 && m_currentIndex < m_tabs.size() ? m_tabs.at(m_currentIndex).id : -1;

    // The controller owns its label, so a browser tab can rename itself to the
    // folder it is showing without the shell knowing what a folder is.
    connect(tab.controller, &FeatureController::titleChanged, this,
        [this, controller = tab.controller, id = tab.id] {
            emitRowChanged(controller, { TitleRole, Qt::DisplayRole });
            // A tab that offers the way back to this one is showing this one's
            // title, so renaming a tab renames every way back to it.
            emitOpenerTitleChanged(id);
        });
    connect(tab.controller, &FeatureController::subtitleChanged, this,
        [this, controller = tab.controller] { emitRowChanged(controller, { SubtitleRole }); });
    // Long jobs -- a report over a large tree -- must be visible from the tab
    // strip, or a tab that is still working looks like one that finished.
    connect(tab.controller, &FeatureController::busyChanged, this,
        [this, controller = tab.controller] { emitRowChanged(controller, { BusyRole }); });
    connect(tab.controller, &FeatureController::stateChanged, this, &TabsModel::sessionDirty);

    const int row = static_cast<int>(m_tabs.size());
    beginInsertRows({}, row, row);
    m_tabs.append(tab);
    endInsertRows();

    emit countChanged();
    emit tabOpened(row);
    setCurrentIndex(row);
    emit sessionDirty();
    return row;
}

void TabsModel::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;

    const bool closingCurrent = index == m_currentIndex;
    const int openerId = m_tabs.at(index).openedFromId;

    beginRemoveRows({}, index, index);
    Tab tab = m_tabs.takeAt(index);
    endRemoveRows();

    if (tab.controller)
        tab.controller->deleteLater();

    emit countChanged();
    emit sessionDirty();
    // Whatever was opened from it has nowhere to go back to now.
    emitOpenerTitleChanged(tab.id);

    if (m_tabs.isEmpty()) {
        selectRow(-1);
        return;
    }

    // Closing the tab you are looking at hands you back the one you opened it
    // from, not whichever tab happens to sit next to it. Open a tab from tab 1
    // while tab 2 exists and the new one lands at the end; by position alone
    // you would return to tab 2, somewhere you were never working.
    int next = -1;
    if (closingCurrent && openerId >= 0)
        next = rowOfTabId(openerId);
    if (next < 0)
        next = m_currentIndex > index ? m_currentIndex - 1 : m_currentIndex;

    selectRow(qBound(0, next, static_cast<int>(m_tabs.size()) - 1));
}

void TabsModel::closeCurrentTab()
{
    closeTab(m_currentIndex);
}

void TabsModel::setCurrentIndex(int index)
{
    const int clamped = m_tabs.isEmpty() ? -1 : qBound(0, index, static_cast<int>(m_tabs.size()) - 1);
    if (m_currentIndex == clamped)
        return;
    m_currentIndex = clamped;
    emit currentIndexChanged();
    emit sessionDirty();
}

Session TabsModel::captureSession() const
{
    Session session;
    session.currentIndex = m_currentIndex;
    for (const Tab& tab : m_tabs) {
        session.tabs.append(
            TabSession { tab.featureId, tab.controller ? tab.controller->saveState() : QVariantMap {} });
    }
    return session;
}

int TabsModel::restoreSession(const Session& session)
{
    int restored = 0;
    for (const TabSession& tab : session.tabs) {
        // A feature merged into another leaves its id in every session written
        // before the merge. Those tabs reopen as their successor; only a tab
        // whose feature nobody provides at all is skipped.
        const int row = openTab(m_registry ? m_registry->currentIdFor(tab.featureId) : tab.featureId);
        if (row < 0)
            continue; // the plugin that provided this tab is gone
        if (auto* controller = m_tabs.at(row).controller)
            controller->restoreState(tab.state);
        ++restored;
    }

    if (restored > 0)
        setCurrentIndex(session.currentIndex >= 0 ? session.currentIndex : 0);
    return restored;
}

QObject* TabsModel::controllerAt(int index) const
{
    if (index < 0 || index >= m_tabs.size())
        return nullptr;
    return m_tabs.at(index).controller;
}

QObject* TabsModel::currentController() const
{
    return controllerAt(m_currentIndex);
}

int TabsModel::rowOfFeature(const QString& featureId) const
{
    for (int row = 0; row < m_tabs.size(); ++row) {
        if (m_tabs.at(row).featureId == featureId)
            return row;
    }
    return -1;
}

int TabsModel::rowOpenedFromCurrent(const QString& featureId) const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_tabs.size())
        return -1;

    const int currentId = m_tabs.at(m_currentIndex).id;
    for (int row = 0; row < m_tabs.size(); ++row) {
        const Tab& tab = m_tabs.at(row);
        if (tab.openedFromId == currentId && tab.featureId == featureId)
            return row;
    }
    return -1;
}

int TabsModel::openerRow(int index) const
{
    if (index < 0 || index >= m_tabs.size())
        return -1;
    return rowOfTabId(m_tabs.at(index).openedFromId);
}

void TabsModel::emitRowChanged(const FeatureController* controller, const QList<int>& roles)
{
    for (int row = 0; row < m_tabs.size(); ++row) {
        if (m_tabs.at(row).controller == controller) {
            const QModelIndex idx = index(row, 0);
            emit dataChanged(idx, idx, roles);
            return;
        }
    }
}

void TabsModel::emitOpenerTitleChanged(int openerId)
{
    if (openerId < 0)
        return;
    for (int row = 0; row < m_tabs.size(); ++row) {
        if (m_tabs.at(row).openedFromId != openerId)
            continue;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, { OpenerTitleRole });
    }
}

int TabsModel::rowOfTabId(int id) const
{
    for (int row = 0; row < m_tabs.size(); ++row) {
        if (m_tabs.at(row).id == id)
            return row;
    }
    return -1;
}

void TabsModel::selectRow(int index)
{
    // The row number can be unchanged while the tab at it is not, so the
    // stored index is invalidated first and the notification forced.
    m_currentIndex = -2;
    setCurrentIndex(index);
}

} // namespace mole
