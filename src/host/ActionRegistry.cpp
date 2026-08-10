#include "host/ActionRegistry.h"

#include <QVariantMap>

#include <algorithm>
#include <array>

namespace mole {

ActionRegistry::ActionRegistry(QObject* parent)
    : QObject(parent)
{
}

ActionRegistry::~ActionRegistry() = default;

QString ActionRegistry::sectionTitle(MenuAction::Section section)
{
    switch (section) {
    case MenuAction::Section::File:
        return QStringLiteral("File");
    case MenuAction::Section::View:
        return QStringLiteral("View");
    case MenuAction::Section::Bookmarks:
        return QStringLiteral("Bookmarks");
    case MenuAction::Section::Operations:
        return QStringLiteral("Operations");
    case MenuAction::Section::Workflows:
        return QStringLiteral("Workflows");
    case MenuAction::Section::Help:
        return QStringLiteral("Help");
    }
    return {};
}

bool ActionRegistry::addAction(MenuAction action)
{
    if (action.id.isEmpty() || action.title.isEmpty() || !action.trigger)
        return false;
    if (contains(action.id))
        return false;

    m_actions.push_back(std::move(action));
    return true;
}

bool ActionRegistry::contains(const QString& id) const
{
    return std::any_of(
        m_actions.begin(), m_actions.end(), [&id](const MenuAction& action) { return action.id == id; });
}

bool ActionRegistry::removeAction(const QString& id)
{
    for (auto it = m_actions.begin(); it != m_actions.end(); ++it) {
        if (it->id == id) {
            m_actions.erase(it);
            return true;
        }
    }
    return false;
}

int ActionRegistry::removeActionsStartingWith(const QString& prefix)
{
    const auto removed = std::remove_if(m_actions.begin(), m_actions.end(),
        [&prefix](const MenuAction& action) { return action.id.startsWith(prefix); });
    const int count = static_cast<int>(std::distance(removed, m_actions.end()));
    m_actions.erase(removed, m_actions.end());
    return count;
}

bool ActionRegistry::trigger(const QString& id)
{
    for (const MenuAction& action : m_actions) {
        if (action.id != id)
            continue;
        // A disabled entry can still be reached by a stale click or a
        // shortcut, so the check belongs here and not only in the view.
        if (action.enabled && !action.enabled())
            return false;
        action.trigger();
        return true;
    }
    return false;
}

QVariantList ActionRegistry::buildModel() const
{
    // The order the headings appear in. Operations before Workflows because the
    // shorter, more frequent list should not be read past to reach the other.
    const std::array<MenuAction::Section, 6> order { MenuAction::Section::File, MenuAction::Section::View,
        MenuAction::Section::Operations, MenuAction::Section::Workflows, MenuAction::Section::Bookmarks,
        MenuAction::Section::Help };

    QVariantList sections;
    for (MenuAction::Section section : order) {
        std::vector<const MenuAction*> inSection;
        for (const MenuAction& action : m_actions) {
            if (action.section == section)
                inSection.push_back(&action);
        }
        if (inSection.empty())
            continue;

        std::stable_sort(inSection.begin(), inSection.end(),
            [](const MenuAction* a, const MenuAction* b) { return a->sortOrder < b->sortOrder; });

        QVariantList entries;
        bool first = true;
        for (const MenuAction* action : inSection) {
            QVariantMap entry;
            entry[QStringLiteral("id")] = action->id;
            entry[QStringLiteral("title")] = action->title;
            entry[QStringLiteral("shortcut")] = action->shortcut;
            entry[QStringLiteral("iconText")] = action->iconText;
            // A divider at the very top of a menu is just a stray line.
            entry[QStringLiteral("separatorBefore")] = action->separatorBefore && !first;
            entry[QStringLiteral("checkable")] = static_cast<bool>(action->checked);
            entry[QStringLiteral("checked")] = action->checked ? action->checked() : false;
            entry[QStringLiteral("enabled")] = action->enabled ? action->enabled() : true;
            // Empty for most entries. Carried through so a test can hold the rule
            // that every registered feature is reachable by some action, now that
            // most of them have no "New … tab" entry -- see ADR-0032.
            entry[QStringLiteral("opensFeature")] = action->opensFeature;
            entries.append(entry);
            first = false;
        }

        QVariantMap group;
        group[QStringLiteral("title")] = sectionTitle(section);
        group[QStringLiteral("actions")] = entries;
        sections.append(group);
    }

    return sections;
}

} // namespace mole
