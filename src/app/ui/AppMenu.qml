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
    // Dimmed rather than washed out: Qt's Material dark theme dims with
    // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
    Overlay.modal: DimVeil {}
    Overlay.modeless: DimVeil {}

    id: appMenu
    objectName: "appMenu"

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
                color: App.colour.textFaint
                // Asked, not named. "monospace" is a fontconfig alias: it
                // resolves to a real family on Linux and to nothing on Windows
                // or macOS, where the label falls back to the default
                // proportional font and the shortcut column stops lining up.
                font.family: App.monospaceFont
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
            color: App.colour.border
        }
    }

    // One component for all five, so they cannot drift apart. They did: only the
    // File submenu declared `focus: true`, which is the kind of difference nobody
    // notices until the keyboard stops halfway along the menu.
    component SectionMenu: Menu {
        id: sectionMenu
        /// The section title as the C++ side names it. The objectName follows it
        /// so a test can address one submenu without a second thing to keep in
        /// step.
        required property string section

        title: sectionMenu.section
        objectName: "menu" + sectionMenu.section
        width: appMenu.widthOf(sectionMenu)
        focus: true

        // Leaving a submenu with Left or Escape closes it and leaves the menu it
        // came from without the keyboard: the arrows then did nothing, which is
        // what made walking this menu impossible without a mouse.
        //
        // Restored on both signals, and deferred on each. `aboutToHide` fires as
        // the close begins and `closed` only after the exit transition -- a fifth
        // of a second later, which is long enough for the next keystroke to fall
        // into the gap. The deferral itself is not optional: Qt moves the focus as
        // part of closing the popup, so anything claimed from inside the handler
        // is taken straight back.
        onAboutToHide: if (appMenu.opened) Qt.callLater(sectionMenu.handBackTheKeyboard)
        onClosed: if (appMenu.opened) Qt.callLater(sectionMenu.handBackTheKeyboard)

        function handBackTheKeyboard() {
            if (!appMenu.opened)
                return
            appMenu.forceActiveFocus()

            // Only when Qt has actually cleared the highlight. Setting it
            // unconditionally would undo a heading the user had already moved to
            // while the transition was still running.
            if (appMenu.currentIndex >= 0)
                return
            for (var i = 0; i < appMenu.count; ++i) {
                const item = appMenu.itemAt(i)
                if (item && item.subMenu === sectionMenu) {
                    appMenu.currentIndex = i
                    return
                }
            }
        }

        Instantiator {
            model: appMenu.entriesFor(sectionMenu.section)
            delegate: ActionItem {}
            onObjectAdded: function(index, object) { sectionMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { sectionMenu.removeItem(object) }
        }
    }

    // The sections the API enum fixes, in the order they appear. Empty ones are
    // skipped when the model is built, so a heading never appears with nothing
    // under it.
    SectionMenu { section: "File" }
    SectionMenu { section: "View" }
    SectionMenu { section: "Operations" }
    SectionMenu { section: "Workflows" }
    SectionMenu { section: "Bookmarks" }
    SectionMenu { section: "Help" }
}
