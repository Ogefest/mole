#pragma once

#include "sdk/MenuAction.h"

#include <QHash>
#include <QObject>
#include <QVariantList>

#include <vector>

namespace mole {

/// Collects menu entries from the shell and from every plugin, and flattens
/// them into something QML can build a menu from.
///
/// The model is rebuilt on demand instead of being kept live: menus are opened
/// rarely and read once, so re-evaluating a handful of predicates costs
/// nothing and removes a whole class of stale-state bugs.
class ActionRegistry : public QObject
{
    Q_OBJECT

public:
    explicit ActionRegistry(QObject* parent = nullptr);
    ~ActionRegistry() override;

    /// Rejects an entry whose id is already taken and returns false.
    bool addAction(MenuAction action);

    bool contains(const QString& id) const;
    /// Removes an entry. Used for lists that change while running, such as the
    /// bookmarks, which are rebuilt rather than kept in sync one row at a time.
    bool removeAction(const QString& id);
    /// Removes every entry whose id starts with `prefix`.
    int removeActionsStartingWith(const QString& prefix);
    /// Runs the entry's trigger. Unknown or disabled ids are ignored.
    bool trigger(const QString& id);

    /// What key reaches `target`, as the window itself declared it. `target` is
    /// an action id, or a feature id for the entries that open a kind of tab.
    ///
    /// **The menu prints a key and the window is what binds it**, and those were
    /// two lists: every label was a string in `buildActions()` -- including a
    /// hash of three feature ids to key text kept for nothing else -- while the
    /// accelerators that actually fire are `Shortcut` items in QML. A copy is
    /// wrong the moment either side moves, and it had been: MOLE-396 found five
    /// entries advertising a key that was not bound or did something else, and
    /// one key printed beside two entries. So the declaration is the source, and
    /// this is where it is handed over -- `nativeText` from the Shortcut, so a
    /// platform that spells `StandardKey.AddTab` as ⌘T is spelled that way in the
    /// menu too rather than as the "Ctrl+T" somebody typed on Linux. See
    /// MOLE-416 and ADR-0032.
    ///
    /// Applied when the menu is built rather than when the action is registered:
    /// QML loads after the actions do, so at registration there is nothing to
    /// read yet.
    ///
    /// What is left in `buildActions()` is the two labels that are not keys --
    /// "type to filter", "type to find, or /". Those are not a copy of anything:
    /// no accelerator exists for them, and the label is how somebody learns that
    /// typing is the way in.
    void declareShortcut(const QString& target, const QString& nativeText);

    /// Sections in display order, each as
    /// `{ title, actions: [{ id, title, shortcut, iconText, separatorBefore,
    ///                       checkable, checked, enabled, opensFeature }] }`.
    /// Empty sections are dropped.
    QVariantList buildModel() const;

    static QString sectionTitle(MenuAction::Section section);

private:
    std::vector<MenuAction> m_actions;
    /// action or feature id -> the key the window declared for it. See
    /// declareShortcut().
    QHash<QString, QString> m_declaredShortcuts;
};

} // namespace mole
