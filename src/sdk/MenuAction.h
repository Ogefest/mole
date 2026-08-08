#pragma once

#include <QString>

#include <functional>

namespace mole {

/// One entry in the application menu.
///
/// The menu is an extension point like everything else: a plugin that adds a
/// duplicate finder also adds its "Find duplicates here" entry under Tools,
/// and the shell places it without knowing what it does.
///
/// The callbacks are evaluated every time the menu is opened rather than
/// cached, so an entry can reflect the current tab without anyone having to
/// remember to invalidate anything.
struct MenuAction
{
    /// Where the entry belongs. Kept deliberately small -- a menu with eleven
    /// top-level headings is not navigation, it is a search problem.
    enum class Section {
        File, ///< creating and closing things
        View, ///< how the current tab looks
        Bookmarks, ///< saved places, and adding the current one
        Tools, ///< operations on files; most plugins land here
        Help
    };

    /// Stable, namespaced identifier, e.g. "org.example.duplicates.scan".
    QString id;
    Section section = Section::Tools;
    QString title;
    /// Shown right-aligned. Display only -- register the real Shortcut in QML.
    QString shortcut;
    QString iconText;
    /// Lower sorts first. Built-ins leave gaps so plugins can slot between.
    int sortOrder = 500;
    /// Draws a divider above this entry.
    bool separatorBefore = false;

    /// What to do when picked.
    std::function<void()> trigger;
    /// Optional. When set, the entry is drawn as a tick box.
    std::function<bool()> checked;
    /// Optional. When it returns false the entry is greyed out rather than
    /// hidden, so the menu does not change shape as you use it.
    std::function<bool()> enabled;
};

} // namespace mole
