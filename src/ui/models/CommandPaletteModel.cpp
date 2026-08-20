#include "ui/models/CommandPaletteModel.h"

#include "host/ActionRegistry.h"
#include "ui/models/BookmarkModel.h"
#include "ui/models/DriveListModel.h"

#include <algorithm>

namespace mole {

CommandPaletteModel::CommandPaletteModel(
    ActionRegistry* actions, BookmarkModel* bookmarks, DriveListModel* drives, QObject* parent)
    : QAbstractListModel(parent)
    , m_actions(actions)
    , m_bookmarks(bookmarks)
    , m_drives(drives)
{
}

int CommandPaletteModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QVariant CommandPaletteModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const Command& command = m_visible.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return command.title;
    case GroupRole:
        return command.group;
    case PathRole:
        return command.path();
    case ShortcutRole:
        return command.shortcut;
    case IconTextRole:
        return command.iconText;
    default:
        return {};
    }
}

QHash<int, QByteArray> CommandPaletteModel::roleNames() const
{
    return {
        { TitleRole, "title" },
        { GroupRole, "group" },
        { PathRole, "path" },
        { ShortcutRole, "shortcut" },
        { IconTextRole, "iconText" },
    };
}

void CommandPaletteModel::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    rebuildVisible();
}

void CommandPaletteModel::refresh()
{
    m_all.clear();

    // The menu, exactly as the menu would show it -- including the enabled
    // callbacks being evaluated now rather than when the action was registered.
    if (m_actions) {
        const QVariantList sections = m_actions->buildModel();
        for (const QVariant& sectionEntry : sections) {
            const QVariantMap section = sectionEntry.toMap();
            const QString group = section.value(QStringLiteral("title")).toString();
            const QVariantList entries = section.value(QStringLiteral("actions")).toList();
            for (const QVariant& actionEntry : entries) {
                const QVariantMap action = actionEntry.toMap();
                // A greyed-out entry is not something that can be done, so it is
                // not offered. Running into "nothing happened" from a list of
                // things you can do is worse than the entry being absent.
                if (!action.value(QStringLiteral("enabled"), true).toBool())
                    continue;
                m_all.append(Command { action.value(QStringLiteral("title")).toString(), group,
                    action.value(QStringLiteral("shortcut")).toString(),
                    action.value(QStringLiteral("iconText")).toString(),
                    action.value(QStringLiteral("id")).toString(), QString(), QString() });
            }
        }
    }

    if (m_bookmarks) {
        for (int row = 0; row < m_bookmarks->rowCount(); ++row) {
            const QModelIndex index = m_bookmarks->index(row, 0);
            Command command { m_bookmarks->data(index, BookmarkModel::NameRole).toString(),
                QStringLiteral("Bookmarks"), QString(), QStringLiteral("☆"), QString(),
                m_bookmarks->data(index, BookmarkModel::TargetRole).toString(), QString() };
            command.bookmarkKind = m_bookmarks->data(index, BookmarkModel::KindRole).toString();
            m_all.append(std::move(command));
        }
    }

    if (m_drives) {
        for (int row = 0; row < m_drives->rowCount(); ++row) {
            const QModelIndex index = m_drives->index(row, 0);
            const QString name = m_drives->data(index, DriveListModel::DisplayNameRole).toString();
            const QString icon = m_drives->data(index, DriveListModel::IconTextRole).toString();
            m_all.append(Command { name, QStringLiteral("Drives"), QString(), icon, QString(),
                m_drives->data(index, DriveListModel::RootUriRole).toString(), QString() });

            // What can be done to it, as against where it goes. Read from the
            // drive list every time, so a drive that has just connected offers
            // Eject and not Connect without anything keeping a second list of
            // verbs in step.
            const QString driveId = m_drives->data(index, DriveListModel::ConfiguredIdRole).toString();
            if (driveId.isEmpty())
                continue; // a local disk has nothing to connect or eject

            const auto offer = [&](const QString& title, DriveCommand verb) {
                m_all.append(Command {
                    title, QStringLiteral("Drives"), QString(), icon, QString(), QString(), driveId, verb });
            };

            if (m_drives->data(index, DriveListModel::CanUnlockRole).toBool()) {
                // Not "Connect", which would fail, and not silence either: the
                // thing in the way is a passphrase and the entry says so.
                offer(QStringLiteral("Unlock the credential store for %1").arg(name), DriveCommand::Unlock);
            } else if (m_drives->data(index, DriveListModel::CanConnectRole).toBool()) {
                offer(QStringLiteral("Connect %1").arg(name), DriveCommand::Connect);
            } else if (m_drives->data(index, DriveListModel::CanEjectRole).toBool()) {
                offer(QStringLiteral("Eject %1").arg(name), DriveCommand::Eject);
            }

            // Always. "Is it actually there" is a fair question whatever the
            // row currently claims -- that is the whole point of the check.
            offer(QStringLiteral("Check %1").arg(name), DriveCommand::Check);
        }
    }

    rebuildVisible();
}

int CommandPaletteModel::score(const Command& command, const QString& needle)
{
    if (needle.isEmpty())
        return 0;

    const QString title = command.title;
    const int inTitle = title.indexOf(needle, 0, Qt::CaseInsensitive);
    // A match on the title beats a match on the group, or typing "set" would bury
    // "Add to set" under everything in a section whose name happens to contain it.
    if (inTitle == 0)
        return 1000;
    if (inTitle > 0)
        return 800 - inTitle;

    if (command.group.contains(needle, Qt::CaseInsensitive))
        return 400;

    // Last: the words of the query found anywhere in the path, in any order, so
    // "op term" finds Operations → Terminal here.
    const QStringList words = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() > 1) {
        const QString path = command.path();
        const bool all = std::all_of(words.cbegin(), words.cend(),
            [&path](const QString& word) { return path.contains(word, Qt::CaseInsensitive); });
        if (all)
            return 200;
    }
    return -1;
}

void CommandPaletteModel::rebuildVisible()
{
    beginResetModel();
    m_visible.clear();

    const QString needle = m_filter.trimmed();
    QList<QPair<int, Command>> ranked;
    for (const Command& command : m_all) {
        const int rank = score(command, needle);
        if (rank >= 0)
            ranked.append({ rank, command });
    }

    // Stable, so equally good matches keep the order the registries gave them --
    // which is the order the menu shows, and therefore the order a reader expects.
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const QPair<int, Command>& a, const QPair<int, Command>& b) { return a.first > b.first; });

    m_visible.reserve(ranked.size());
    for (const QPair<int, Command>& entry : ranked)
        m_visible.append(entry.second);

    endResetModel();
    emit countChanged();
}

void CommandPaletteModel::activate(int row)
{
    if (row < 0 || row >= m_visible.size())
        return;
    const Command& command = m_visible.at(row);

    if (!command.actionId.isEmpty()) {
        emit actionRequested(command.actionId);
        return;
    }
    if (!command.bookmarkKind.isEmpty()) {
        emit bookmarkRequested(command.bookmarkKind, command.uri);
        return;
    }
    if (!command.uri.isEmpty()) {
        emit locationRequested(command.uri);
        return;
    }
    if (!command.driveId.isEmpty())
        emit driveCommandRequested(command.verb, command.driveId);
}

} // namespace mole
