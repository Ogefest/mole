import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 1280
    height: 800
    visible: true
    title: "Mole"

    // Restored before the window is shown, so it does not appear at the
    // default size and then jump.
    Component.onCompleted: {
        var saved = App.savedWindowGeometry()
        if (saved.width)
            root.width = saved.width
        if (saved.height)
            root.height = saved.height
        // Position is only offered when it still lands on a screen that
        // exists; an unplugged monitor would put the window out of reach.
        if (saved.x !== undefined && saved.y !== undefined) {
            root.x = saved.x
            root.y = saved.y
        }
        if (saved.maximized)
            root.visibility = Window.Maximized
        geometryWatcher.armed = true
    }

    // Coalesced: dragging a window edge fires a change per pixel.
    Timer {
        id: geometryWatcher
        property bool armed: false
        interval: 400
        onTriggered: App.rememberWindowGeometry(root.x, root.y, root.width, root.height,
                                                root.visibility === Window.Maximized)
    }

    onWidthChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onHeightChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onXChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onYChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onVisibilityChanged: if (geometryWatcher.armed) geometryWatcher.restart()

    Material.theme: Material.Dark
    Material.primary: "#1f2430"
    Material.accent: "#4c9aff"
    Material.background: "#151922"

    readonly property color panelColor: "#1b2029"
    readonly property color borderColor: "#2a3140"
    readonly property color mutedText: "#8b93a7"

    // The shell knows nothing about what a tab does. It asks the registry what
    // exists, and loads whatever QML each feature points at.
    function openFeature(featureId) {
        App.openFeatureTab(featureId)
    }

    // Puts the keyboard back where it belongs. A window manager decides who
    // gets focus when the window is activated, and its answer -- the first
    // focusable control, which is the path bar -- is not ours.
    function focusCurrentTab() {
        var loader = tabStack.itemAt(App.tabs.currentIndex)
        if (loader && loader.item && loader.item.focusActivePane)
            loader.item.focusActivePane()
    }

    onActiveChanged: if (active) Qt.callLater(focusCurrentTab)

    // Set MOLE_DEBUG_FOCUS=1 to have the window report what holds the keyboard.
    // Focus problems depend on the window manager, so this beats guessing.
    Timer {
        running: Qt.application.arguments.indexOf("--debug-focus") >= 0
                 || Qt.application.arguments.indexOf("--debug-keys") >= 0
        interval: 1000
        repeat: true
        onTriggered: {
            var item = root.activeFocusItem
            console.log("[focus]", item ? item.toString() : "<nothing>",
                        "typing:", item && item.selectedText !== undefined)
        }
    }

    Connections {
        target: App.tabs
        function onCurrentIndexChanged() { Qt.callLater(root.focusCurrentTab) }
    }

    function cycleTab(delta) {
        if (App.tabs.count === 0)
            return
        var next = (App.tabs.currentIndex + delta + App.tabs.count) % App.tabs.count
        App.tabs.currentIndex = next
    }

    // Window-level shortcuts. Anything that depends on which pane has focus
    // is handled inside FilePane instead -- a key only means something
    // relative to what it is aimed at.
    Shortcut {
        sequences: [StandardKey.AddTab]          // Ctrl+T
        onActivated: root.openFeature("mole.browser")
    }
    Shortcut {
        sequence: "Ctrl+Shift+T"
        onActivated: root.openFeature("mole.commander")
    }
    Shortcut {
        sequences: [StandardKey.Close]           // Ctrl+W
        onActivated: App.tabs.closeCurrentTab()
    }
    Shortcut {
        sequences: [StandardKey.Find]            // Ctrl+F
        onActivated: root.openFeature("mole.livesearch")
    }
    Shortcut {
        // The one key that reaches everything, and the key everyone's fingers
        // already know from an editor.
        sequence: "Ctrl+Shift+P"
        onActivated: commandPalette.open()
    }
    Shortcut {
        // Measures the folders in front of you, in the background, and writes the
        // answers into the listing.
        sequence: "Ctrl+Shift+S"
        onActivated: App.triggerAction("mole.tools.folderSizes")
    }
    Shortcut {
        sequence: "Ctrl+Shift+I"
        onActivated: root.openFeature("mole.indexsearch")
    }
    Shortcut {
        sequences: [StandardKey.NextChild, "Ctrl+PgDown"]
        onActivated: root.cycleTab(1)
    }
    Shortcut {
        sequences: [StandardKey.PreviousChild, "Ctrl+PgUp"]
        onActivated: root.cycleTab(-1)
    }
    Shortcut {
        sequences: [StandardKey.Quit]            // Ctrl+Q
        onActivated: Qt.quit()
    }
    Shortcut {
        // F4 is the classic "open the menu" key in a commander, and it makes
        // the whole menu reachable without the mouse.
        sequence: "F4"
        onActivated: {
            appMenu.popup(menuButton, 0, menuButton.height)
            appMenu.forceActiveFocus()
        }
    }
    Shortcut {
        sequence: "Ctrl+D"
        onActivated: App.triggerAction("mole.bookmarks.add")
    }
    Shortcut {
        // Ctrl+G to type a destination; Ctrl+L is the same thing under the
        // name a browser would use.
        sequences: ["Ctrl+G", "Ctrl+L"]
        onActivated: {
            var loader = tabStack.itemAt(App.tabs.currentIndex)
            if (loader && loader.item && loader.item.focusPathBar)
                loader.item.focusPathBar()
        }
    }
    DrivesDialog {
        id: drivesDialog
    }

    Connections {
        target: App
        function onDrivesRequested() { drivesDialog.open() }
    }

    Shortcut {
        // The key every IDE uses for this, and one no listing wants.
        sequence: "Ctrl+`"
        onActivated: App.triggerAction("mole.tools.terminal")
    }
    Shortcut {
        sequences: [StandardKey.HelpContents]    // F1
        onActivated: shortcutDialog.open()
    }
    Shortcut {
        // Ctrl+R only. StandardKey.Refresh also means F5 on this platform, and
        // F5 is the commander copy key -- the window shortcut was swallowing it
        // before the pane ever saw it, so F5 refreshed instead of copying.
        sequence: "Ctrl+R"
        onActivated: App.triggerAction("mole.view.refresh")
    }

    // Ctrl+1..9 jumps straight to a tab.
    Repeater {
        model: 9
        delegate: Item {
            required property int index
            Shortcut {
                sequence: "Ctrl+" + (index + 1)
                onActivated: if (index < App.tabs.count) App.tabs.currentIndex = index
            }
        }
    }

    header: ToolBar {
        Material.background: root.panelColor
        // Nothing in the toolbar is a keyboard destination; leaving these
        // focusable is how the keyboard ends up somewhere it can do nothing.
        focusPolicy: Qt.NoFocus

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 8
            spacing: 8

            // Everything that used to be a row of buttons up here now lives in
            // the menu. The buttons duplicated the sidebar and left nowhere to
            // put the features still to come.
            ToolButton {
                id: menuButton
                objectName: "menuButton"
                text: "☰"
                font.pixelSize: 18
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered && !appMenu.visible
                ToolTip.text: "Menu"
                onClicked: appMenu.popup(menuButton, 0, menuButton.height)
            }

            Label {
                text: "Mole"
                font.bold: true
                font.pixelSize: 15
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: App.tasks.activeCount > 0
                text: App.tasks.activeCount + " running"
                color: root.mutedText
            }

            BusyIndicator {
                running: App.tasks.activeCount > 0
                visible: running
                implicitWidth: 22
                implicitHeight: 22
            }
        }
    }

    AppMenu { id: appMenu }
    CommandPalette { id: commandPalette }

    // Last stop for navigation keys.
    //
    // The pane handles these itself when it has the keyboard. This is the
    // safety net for when it does not -- focus sitting on the sidebar, the tab
    // strip, or wherever a window manager decided to put it -- because a file
    // manager where Enter does nothing is broken no matter whose fault the
    // focus is.
    Item {
        id: keyFallback
        anchors.fill: parent

        // Never steal a key from something the user is typing into. Text
        // inputs are the ones that answer to `selectedText`.
        function typingSomewhere() {
            var item = root.activeFocusItem
            return item !== null && item.selectedText !== undefined
        }

        function currentView() {
            var loader = tabStack.itemAt(App.tabs.currentIndex)
            return loader ? loader.item : null
        }

        readonly property bool debugKeys:
            Qt.application.arguments.indexOf("--debug-keys") >= 0

        Keys.onPressed: function(event) {
            if (keyFallback.debugKeys) {
                var held = root.activeFocusItem
                console.log("[window] key=" + event.key,
                            "focus=" + (held ? held.toString() : "<nothing>"),
                            "typing=" + keyFallback.typingSomewhere())
            }
            if (keyFallback.typingSomewhere())
                return
            var view = keyFallback.currentView()
            if (!view)
                return

            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (view.activateCurrentRow) {
                    view.activateCurrentRow()
                    event.accepted = true
                }
                break
            case Qt.Key_Down:
                if (view.moveCursorBy) {
                    view.moveCursorBy(1)
                    event.accepted = true
                }
                break
            case Qt.Key_Up:
                if (view.moveCursorBy) {
                    view.moveCursorBy(-1)
                    event.accepted = true
                }
                break
            case Qt.Key_Backspace:
                if (view.goUpOneFolder) {
                    view.goUpOneFolder()
                    event.accepted = true
                }
                break
            }
        }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        Sidebar {
            SplitView.preferredWidth: 240
            SplitView.minimumWidth: 160
            panelColor: root.panelColor
            borderColor: root.borderColor
            mutedText: root.mutedText
        }

        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0

            TabBar {
                id: tabBar
                Layout.fillWidth: true
                Material.background: root.panelColor
                currentIndex: App.tabs.currentIndex
                onCurrentIndexChanged: App.tabs.currentIndex = currentIndex

                Repeater {
                    model: App.tabs
                    delegate: TabButton {
                        id: tabButton
                        required property int index
                        required property string title
                        required property string iconText
                        required property bool busy

                        width: Math.max(140, implicitContentWidth + 48)
                        contentItem: RowLayout {
                            spacing: 6

                            // A tab still working says so in place of its icon.
                            // A report over a large tree can run for minutes,
                            // and a finished tab and a working one must not
                            // look the same.
                            Item {
                                implicitWidth: 16
                                implicitHeight: 16
                                Label {
                                    anchors.centerIn: parent
                                    text: tabButton.iconText
                                    visible: !tabButton.busy
                                }
                                BusyIndicator {
                                    objectName: "tabBusy"
                                    anchors.centerIn: parent
                                    width: 16
                                    height: 16
                                    running: tabButton.busy
                                    visible: tabButton.busy
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: tabButton.title
                                elide: Text.ElideMiddle
                            }
                            ToolButton {
                                objectName: "closeTabButton"
                                text: "×"
                                font.pixelSize: App.textSize
                                implicitWidth: App.minimumTarget
                                implicitHeight: App.minimumTarget
                                onClicked: App.tabs.closeTab(index)
                            }
                        }
                    }
                }
            }

            // Every open tab keeps its own loaded view, so switching back is
            // instant and a running search is not thrown away.
            StackLayout {
                id: tabStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Hidden rather than merely empty: a visible layout with
                // fillHeight still claims its share, which pushed the empty
                // message into the lower half of the window.
                visible: App.tabs.count > 0
                currentIndex: App.tabs.currentIndex

                Repeater {
                    model: App.tabs
                    delegate: Loader {
                        required property url viewSource
                        required property var controller

                        asynchronous: false
                        source: viewSource
                        onLoaded: {
                            if (item)
                                item.controller = controller
                        }
                        // Keep the keyboard with the tab the user is looking at.
                        onVisibleChanged: if (visible && item && item.focusActivePane)
                                              Qt.callLater(item.focusActivePane)
                    }
                }
            }

            // Closing the last tab leaves a blank rectangle that reads as a
            // broken window rather than an empty one.
            //
            // Anchored rather than laid out: a ColumnLayout is only as wide as
            // its widest child, so centring inside it put the block off to one
            // side of a wide window instead of in the middle of it.
            Item {
                objectName: "emptyState"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: App.tabs.count === 0

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 680)
                    spacing: 10

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "No tabs open"
                    font.pixelSize: 18
                    color: root.mutedText
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    Layout.maximumWidth: 420
                    color: "#6f7788"
                    font.pixelSize: 12
                    text: "Every tab is one way of working with files: browsing, comparing two "
                        + "folders side by side, searching, or analysing what a folder is made of."
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 6
                    spacing: 8

                    Repeater {
                        model: App.features
                        delegate: Button {
                            required property string featureId
                            required property string title
                            required property string description
                            required property bool needsContext
                            // A preview needs a file and a report needs a
                            // folder; with nothing open there is neither, and
                            // a button that opens an empty tab reads as broken
                            // rather than as inapplicable.
                            visible: !needsContext
                            text: title
                            flat: true
                            font.pixelSize: 12
                            focusPolicy: Qt.NoFocus
                            ToolTip.visible: hovered
                            ToolTip.text: description
                            onClicked: root.openFeature(featureId)
                        }
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 4
                    text: "Ctrl+T for a browser · F4 for the menu"
                    color: "#5c6472"
                    font.pixelSize: 11
                }

                }
            }

            // Split rather than overlaid: a terminal that covered the listing
            // would defeat the point of having both on screen at once.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                visible: App.terminal.visible
                color: root.borderColor
            }

            TerminalPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(root.height * 0.32)
                visible: App.terminal.visible
                terminal: App.terminal
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: root.borderColor
            }

            TaskStrip {
                Layout.fillWidth: true
                panelColor: root.panelColor
                mutedText: root.mutedText
            }
        }
    }
    }

    Dialog {
        id: aboutDialog
        title: "Loaded plugins"
        modal: true
        anchors.centerIn: parent
        width: Math.min(560, root.width - 80)
        standardButtons: Dialog.Close

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Repeater {
                model: App.pluginSummary
                delegate: Label {
                    required property string modelData
                    text: "•  " + modelData
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                implicitHeight: 1
                color: root.borderColor
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: root.mutedText
                text: "Mole is free software under Apache-2.0.\n"
                    + "It uses the Qt framework under the LGPL-3.0, dynamically linked and "
                    + "unmodified, and libarchive under BSD-2-Clause.\n"
                    + "Full texts ship alongside the application; see LICENSING.md for how to "
                    + "replace Qt with your own build."
            }

            Label {
                visible: App.pluginErrors.length > 0
                text: "Problems"
                font.bold: true
                topPadding: 8
            }

            Repeater {
                model: App.pluginErrors
                delegate: Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Material.color(Material.Red)
                    text: modelData
                }
            }
        }
    }

    Dialog {
        id: shortcutDialog
        title: "Keyboard"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, root.width - 80)
        standardButtons: Dialog.Close

        readonly property var groups: [
            { "heading": "Tabs", "keys": [
                ["Ctrl+T", "New browser tab"],
                ["Ctrl+Shift+T", "New dual-pane tab"],
                ["just start typing", "Filter this folder"],
                ["Ctrl+F", "Search a whole tree (new tab)"],
                ["Ctrl+Shift+I", "Search the index (new tab)"],
                ["F3", "Preview the file under the cursor"],
                ["Ctrl+Shift+A", "Analyse the selected folders, or this one"],
                ["Ctrl+Shift+P", "Find any command, bookmark or drive"],
                ["Ctrl+Shift+S", "Size the selected folders, or all of them"],
                ["Ctrl+W", "Close tab"],
                ["Ctrl+Tab / Ctrl+PgDn", "Next tab"],
                ["Ctrl+1 … Ctrl+9", "Jump to tab"]]},
            { "heading": "Moving around", "keys": [
                ["↑ ↓ / PgUp / PgDn", "Move the cursor"],
                ["Home / End", "First / last entry"],
                ["Enter", "Open — folder, archive, or the default application"],
                ["Ctrl+← / Ctrl+→", "Back / forward"],
                ["Ctrl+↑ or Backspace", "Go up one folder"],
                ["Ctrl+G", "Type a destination"],
                ["Tab", "Switch pane (dual mode)"]]},
            { "heading": "Selecting", "keys": [
                ["Insert", "Tick and step down"],
                ["Space", "Tick without moving"],
                ["Ctrl+A", "Select all"],
                ["*", "Invert selection"]]},
            { "heading": "Files", "keys": [
                ["F2", "Rename"],
                ["F5 / F6", "Copy / move to the other pane"],
                ["F7", "New folder"],
                ["F8 or Delete", "Delete"]]}
        ]

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Repeater {
                model: shortcutDialog.groups
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        text: modelData.heading
                        font.bold: true
                        color: Material.accent
                        font.pixelSize: 12
                    }

                    Repeater {
                        model: modelData.keys
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label {
                                Layout.preferredWidth: 180
                                text: modelData[0]
                                font.family: "monospace"
                                font.pixelSize: 12
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData[1]
                                color: "#8b93a7"
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: App
        function onDialogRequested(actionId) {
            if (actionId === "mole.view.filter") {
                var loader = tabStack.itemAt(App.tabs.currentIndex)
                if (loader && loader.item && loader.item.focusFilter)
                    loader.item.focusFilter()
            }
            else if (actionId === "mole.help.shortcuts")
                shortcutDialog.open()
            else if (actionId === "mole.help.plugins" || actionId === "mole.help.about")
                aboutDialog.open()
        }
        function onNotification(severity, title, detail) {
            notificationLabel.text = detail.length > 0 ? title + " — " + detail : title
            notificationPopup.open()
        }
    }

    Popup {
        id: notificationPopup
        x: (root.width - width) / 2
        y: root.height - height - 80
        width: Math.min(600, root.width - 100)
        padding: 14
        Material.background: root.panelColor

        Timer {
            running: notificationPopup.opened
            interval: 5000
            onTriggered: notificationPopup.close()
        }

        Label {
            id: notificationLabel
            anchors.fill: parent
            wrapMode: Text.Wrap
        }
    }
}
