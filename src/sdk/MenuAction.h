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
    ///
    /// Choosing between the two middle ones is one question: does the entry *do
    /// something to the files in front of you*, or does it *hand you a tool to
    /// work with*? If it needs a tab of its own to be useful at all, it is a
    /// workflow. See docs/adr/0003-menu-sections.md, which works through the
    /// examples that sound like both.
    enum class Section {
        File, ///< creating and closing things
        /// How what you are looking at looks. The current tab, mostly -- one pane
        /// or two, hidden files, a filter -- and, since MOLE-279, the window's
        /// theme, which is the one entry here that is not about a tab. See
        /// ADR-0073 for why it is not a fifth heading.
        View,
        /// Acts on the selection, or on the current folder when nothing is
        /// selected, and leaves you where you were. "Index this folder."
        Operations,
        /// Opens a tab that is a tool you then work in. "Bulk rename."
        Workflows,
        Bookmarks, ///< saved places, and adding the current one
        Help
    };

    /// Stable, namespaced identifier, e.g. "org.example.duplicates.scan".
    QString id;
    /// A contributed feature tab is the common case, so that is the default.
    Section section = Section::Workflows;
    QString title;
    /// Shown right-aligned. Display only -- register the real Shortcut in QML.
    QString shortcut;
    QString iconText;
    /// Lower sorts first. Built-ins leave gaps so plugins can slot between.
    int sortOrder = 500;
    /// Draws a divider above this entry.
    bool separatorBefore = false;
    /// The feature this entry opens a tab of, when it opens one. Set it: most
    /// features have no "New … tab" entry — see IFeature::opensFromNothing() —
    /// and this is what proves each of them is reachable by some other route
    /// rather than being orphaned by a tidy-up of the menu.
    QString opensFeature;

    /// What to do when picked.
    std::function<void()> trigger;
    /// Optional. When set, the entry is drawn as a tick box.
    std::function<bool()> checked;
    /// Optional. When it returns false the entry is greyed out rather than
    /// hidden, so the menu does not change shape as you use it.
    std::function<bool()> enabled;
};

} // namespace mole
