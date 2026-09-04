#include "host/ActionRegistry.h"

#include <QVariantMap>

#include <algorithm>
#include <array>
#include <functional>

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
    // **Copied out of the registry before either is called.** This used to hold a
    // reference into m_actions and invoke the std::function living in it, so an
    // entry whose handler changes the registry destroyed its own callable while
    // it was on the stack. "Add current folder" does exactly that:
    // Bookmarks::add() emits countChanged, a direct connection rebuilds the
    // bookmark actions, and removeActionsStartingWith("mole.bookmarks.") erases
    // the entry being run. Picking a bookmark is the same shape through
    // openPlace(). Both happen to survive today because of what those particular
    // lambdas capture, and the extension point every plugin's menu entry goes
    // through had no rule against a handler that touches the registry. Copying
    // the std::function copies its captures, which is what keeps them alive.
    // See MOLE-365.
    std::function<void()> run;
    std::function<bool()> enabled;
    for (const MenuAction& action : m_actions) {
        if (action.id != id)
            continue;
        run = action.trigger;
        enabled = action.enabled;
        break;
    }
    if (!run)
        return false;

    // A disabled entry can still be reached by a stale click or a shortcut, so
    // the check belongs here and not only in the view.
    if (enabled && !enabled())
        return false;
    run();
    return true;
}

void ActionRegistry::declareShortcut(const QString& target, const QString& nativeText)
{
    if (target.isEmpty())
        return;
    if (nativeText.isEmpty())
        m_declaredShortcuts.remove(target);
    else
        m_declaredShortcuts.insert(target, nativeText);
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
            // The label comes from whatever declared the key: this entry's own
            // id, or -- for the entries that open a kind of tab -- the feature
            // the key opens. What an action was registered with is the fallback,
            // which is now only the two labels that are not keys at all. See
            // declareShortcut().
            QString key = m_declaredShortcuts.value(action->id);
            if (key.isEmpty() && !action->opensFeature.isEmpty())
                key = m_declaredShortcuts.value(action->opensFeature);
            entry[QStringLiteral("shortcut")] = key.isEmpty() ? action->shortcut : key;
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
