import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The application menu, behind the hamburger.
//
// Its contents come from C++: the shell registers File/View/Help entries and
// every plugin adds its own, so nothing here has to be edited to add a feature.
// The four sections are declared statically because they are fixed by the API
// enum; only the entries inside them are dynamic.
Menu {
    id: appMenu

    property var sections: []

    // Rebuilt on every open so tick boxes and greyed-out entries reflect the
    // tab that is actually in front of the user.
    function refresh() {
        sections = App.buildMenu()
    }

    // Qt Quick Controls does not grow a Menu to fit items with a custom
    // contentItem -- the shortcut column simply gets clipped. Measuring the
    // real items is font-accurate, unlike guessing from character counts.
    function widthOf(menu) {
        var widest = 220
        for (var i = 0; i < menu.count; ++i) {
            var item = menu.itemAt(i)
            if (item && item.implicitWidth > widest)
                widest = item.implicitWidth
        }
        return widest
    }

    function entriesFor(title) {
        for (var i = 0; i < sections.length; ++i) {
            if (sections[i].title === title)
                return sections[i].actions
        }
        return []
    }

    onAboutToShow: refresh()
    // Opened from F4 there is no mouse involved, so the menu has to take the
    // keyboard itself; arrows, Enter and Escape are then Qt's own handling.
    onOpened: forceActiveFocus()
    focus: true

    // One reusable delegate: every entry looks the same whether the shell or a
    // plugin contributed it.
    component ActionItem: MenuItem {
        required property var modelData

        text: modelData.title
        enabled: modelData.enabled
        checkable: modelData.checkable
        checked: modelData.checked
        onTriggered: App.triggerAction(modelData.id)

        // A Control does not widen itself to fit a custom contentItem, so the
        // shortcut would sit on top of the title. The width is stated here and
        // Menu sizes itself to the widest entry.
        implicitWidth: 32 + titleLabel.implicitWidth + 24 + shortcutLabel.implicitWidth + 24

        contentItem: RowLayout {
            spacing: 0

            // Checkable entries get the style's own tick box drawn to the left
            // of this contentItem, so only plain entries need a glyph here.
            Label {
                Layout.preferredWidth: modelData.checkable ? 0 : 24
                visible: !modelData.checkable
                text: modelData.iconText ? modelData.iconText : ""
                color: Material.accent
                font.pixelSize: 12
            }

            Label {
                id: titleLabel
                Layout.fillWidth: true
                Layout.minimumWidth: implicitWidth
                text: modelData.title
                opacity: modelData.enabled ? 1.0 : 0.4
                font.pixelSize: 13
            }

            Label {
                id: shortcutLabel
                Layout.leftMargin: 24
                text: modelData.shortcut
                visible: modelData.shortcut.length > 0
                color: "#6f7788"
                font.family: "monospace"
                font.pixelSize: 11
            }
        }

        // A divider drawn as a child keeps the Instantiator producing one
        // uniform item type; z lifts it above the item's own background.
        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: 1
            z: 1
            visible: modelData.separatorBefore
            color: "#2a3140"
        }
    }

    Menu {
        id: fileMenu
        title: "File"
        width: appMenu.widthOf(fileMenu)
        focus: true
        Instantiator {
            model: appMenu.entriesFor("File")
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { fileMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { fileMenu.removeItem(object) }
        }
    }

    Menu {
        id: viewMenu
        title: "View"
        width: appMenu.widthOf(viewMenu)
        Instantiator {
            model: appMenu.entriesFor("View")
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { viewMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { viewMenu.removeItem(object) }
        }
    }

    Menu {
        id: bookmarksMenu
        title: "Bookmarks"
        width: appMenu.widthOf(bookmarksMenu)
        Instantiator {
            model: appMenu.entriesFor("Bookmarks")
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { bookmarksMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { bookmarksMenu.removeItem(object) }
        }
    }

    Menu {
        id: toolsMenu
        title: "Tools"
        width: appMenu.widthOf(toolsMenu)
        Instantiator {
            model: appMenu.entriesFor("Tools")
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { toolsMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { toolsMenu.removeItem(object) }
        }
    }

    Menu {
        id: helpMenu
        title: "Help"
        width: appMenu.widthOf(helpMenu)
        Instantiator {
            model: appMenu.entriesFor("Help")
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { helpMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { helpMenu.removeItem(object) }
        }
    }
}
