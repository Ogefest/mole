#pragma once

#include "sdk/MenuAction.h"

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

    /// Sections in display order, each as
    /// `{ title, actions: [{ id, title, shortcut, iconText, separatorBefore,
    ///                       checkable, checked, enabled }] }`.
    /// Empty sections are dropped.
    QVariantList buildModel() const;

    static QString sectionTitle(MenuAction::Section section);

private:
    std::vector<MenuAction> m_actions;
};

} // namespace mole
