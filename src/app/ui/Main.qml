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
        if (saved.windowState === "maximized")
            root.visibility = Window.Maximized
        else if (saved.windowState === "fullscreen")
            root.visibility = Window.FullScreen
        geometryWatcher.armed = true
    }

    // Coalesced: dragging a window edge fires a change per pixel.
    Timer {
        id: geometryWatcher
        property bool armed: false
        interval: 400
        // The visibility itself, not a flag derived from it. Reducing it here to
        // "is it maximised" is what lost a full-screen window its size:
        // full-screen is not maximised, so the screen-sized metrics were taken
        // for the ones the user had chosen and written over the real ones.
        // Nothing in the application enters full-screen itself -- the state
        // arrives from the window manager, and this is the only thing that
        // notices it.
        onTriggered: App.rememberWindowGeometry(root.x, root.y, root.width, root.height,
                                                root.visibility)
    }

    onWidthChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onHeightChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onXChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onYChanged: if (geometryWatcher.armed) geometryWatcher.restart()
    onVisibilityChanged: if (geometryWatcher.armed) geometryWatcher.restart()

    // What a Material control draws for itself -- a text field's underline and
    // placeholder, a combo box's popup, a dialog's ground, a ripple, a disabled
    // label's opacity -- comes from these four rather than from a token, so they
    // are bound to tokens instead of stated twice.
    //
    // `theme` is the polarity rather than a colour, and it is the one line that
    // decides everything a control draws without being told. Left on Dark under a
    // light palette the window is white and half the controls in it are not. See
    // ADR-0074.
    Material.theme: App.colour.light ? Material.Light : Material.Dark
    Material.primary: App.colour.panel
    Material.accent: App.colour.accent
    // The window's own ground, not `Material.background`.
    //
    // Setting the latter here looked equivalent and was not: it propagates to
    // every control in the window, and Material asks the *inherited* background
    // for a highlighted button's fill. So the active layout button was painted in
    // the window's colour with white text on it -- invisible the moment the window
    // went light, and a dark pill that looked deliberate before that. A control
    // that wants the palette's ground still asks for it by name. See ADR-0074.
    color: App.colour.window

    // The shell knows nothing about what a tab does. It asks the registry what
    // exists, and loads whatever QML each feature points at.
    function openFeature(featureId) {
        App.openFeatureTab(featureId)
    }

    // The loaded view of the current tab, or null.
    //
    // A tab's delegate is a layout rather than the loader itself, because a
    // browser opened from a search carries the way back to it above its
    // contents. So the view is one level in, and everything that speaks to a
    // tab comes through here rather than reaching into the delegate.
    function currentTabItem() {
        var body = tabStack.itemAt(App.tabs.currentIndex)
        return body ? body.view : null
    }

    // Puts the keyboard back where it belongs. A window manager decides who
    // gets focus when the window is activated, and its answer -- the first
    // focusable control, which is the path bar -- is not ours.
    function focusCurrentTab() {
        var view = root.currentTabItem()
        if (view && view.focusActivePane)
            view.focusActivePane()
    }

    onActiveChanged: if (active) Qt.callLater(focusCurrentTab)

    // `--debug-focus` (or `--debug-keys`) has the window report what holds the
    // keyboard, once a second. Focus problems depend on the window manager, so
    // this beats guessing.
    //
    // This said MOLE_DEBUG_FOCUS=1, which nothing has ever read: the switch is
    // the argument the Timer below actually looks for. See MOLE-398.
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
        // By name rather than by id: App.openFeatureTab("...") with an id
        // nothing knows returns -1 in silence, so a rename broke this key with
        // no diagnostic anywhere. See MOLE-396.
        onActivated: App.openNewBrowser()
    }
    Shortcut {
        sequence: "Ctrl+Shift+T"
        onActivated: App.openNewCommander()
    }
    Shortcut {
        sequences: [StandardKey.Close]           // Ctrl+W
        onActivated: App.tabs.closeCurrentTab()
    }
    Shortcut {
        sequences: [StandardKey.Find]            // Ctrl+F
        onActivated: App.openSearchHere()
    }
    Shortcut {
        // The one key that reaches everything. It used to be Refresh's, which was
        // the wrong use of a key this good: refreshing is one entry in the palette
        // like everything else, and it still has F5's neighbour keys and the View
        // menu.
        sequence: "Ctrl+R"
        onActivated: commandPalette.open()
    }
    Shortcut {
        // Measures the folders in front of you, in the background, and writes the
        // answers into the listing.
        sequence: "Ctrl+Shift+S"
        onActivated: App.triggerAction("mole.tools.folderSizes")
    }
    Shortcut {
        // The same search, asked of everywhere that has been scanned. There used
        // to be a second tab behind this key; the scope it stood for is a field
        // in the one form now, and the key still lands on it.
        sequence: "Ctrl+Shift+I"
        onActivated: App.openSearchEverywhere()
    }
    // The two copy-a-location keys. Bound here rather than left to the menu: an
    // action's `shortcut` is only what the menu prints beside it, so a key named
    // there and not declared here would be advertised and do nothing.
    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: App.triggerAction("mole.path.copyFolder")
    }
    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: App.triggerAction("mole.path.copyFile")
    }
    // **The four the comment above was written about, and which were in exactly
    // that state**: the menu printed them and nothing declared them, so they were
    // advertised and did nothing. Ctrl+Shift+A was in the in-window key list as
    // well. tst_QmlConventions compares the two lists now, so a label without a
    // key here fails a suite rather than reaching somebody's fingers. See
    // MOLE-396.
    Shortcut {
        sequence: "Ctrl+Shift+A"
        onActivated: App.triggerAction("mole.tools.analyse")
    }
    Shortcut {
        sequence: "Ctrl+Shift+R"
        onActivated: App.triggerAction("mole.tools.bulkRename")
    }
    Shortcut {
        sequence: "Ctrl+Shift+L"
        onActivated: App.triggerAction("mole.tools.alerts")
    }
    Shortcut {
        sequence: "Ctrl+Shift+J"
        onActivated: App.triggerAction("mole.tools.automation")
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
        // **Both, and the literal is not decoration.** `QKeySequence::Quit` has
        // no binding at all on X11 -- Qt defines one on macOS and Windows -- so
        // this declared nothing here while the menu printed Ctrl+Q beside Quit.
        // The same shape as the two tab-cycling keys above, and for the same
        // reason. See MOLE-396.
        sequences: [StandardKey.Quit, "Ctrl+Q"]
        onActivated: Qt.quit()
    }
    // --- the keys that act on the pane in front of you ---------------------
    //
    // Window shortcuts, not pane handlers. F3 and Ctrl+Up were handlers on the
    // focused item, so they stopped working the moment the keyboard went
    // anywhere else -- clicking a drive in the sidebar was enough -- and came
    // back only when the listing was clicked. They read as though something
    // else were catching them. Nothing was catching them at all.
    //
    // They belong here because what they act on does not depend on the focus:
    // the active tab's active pane knows its own cursor, which is how the menu
    // entry for Preview has always worked from anywhere. See
    // docs/adr/0019-the-keys-that-belong-to-the-window.md -- and note what
    // stays in FilePane: cursor movement, type-to-filter and selection mean
    // nothing without the keyboard.
    Shortcut {
        // The view is asked first, by name, the way Ctrl+G asks for
        // focusPathBar() and the arrows ask for moveCursorBy(): a tab with a
        // cursor over files but no pane -- a search, a set -- can answer for
        // itself. A browser has no previewCurrentRow() and keeps resolving
        // through its active pane, because the pane's cursor is the tab's own
        // idea of where it is and the view does not own it. See MOLE-204.
        sequence: "F3"
        onActivated: {
            var view = root.currentTabItem()
            if (view && view.previewCurrentRow) {
                view.previewCurrentRow()
                return
            }
            App.previewCurrent()
        }
    }
    Shortcut {
        sequence: "Ctrl+Up"
        onActivated: App.goUpInCurrentPane()
    }
    Shortcut {
        sequence: "Ctrl+Left"
        onActivated: App.goBackInCurrentPane()
    }
    Shortcut {
        sequence: "Ctrl+Right"
        onActivated: App.goForwardInCurrentPane()
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
            var view = root.currentTabItem()
            if (view && view.focusPathBar)
                view.focusPathBar()
        }
    }
    DrivesDialog {
        id: drivesDialog
    }

    // Here rather than in the sidebar, because it is asked for from four places
    // -- opening a locked drive, the key on its row, the palette's Unlock, and
    // the drives dialog -- and none of them should have to know where it lives.
    UnlockDialog {
        id: unlockDialog
    }

    Connections {
        target: App
        function onDrivesRequested() { drivesDialog.open() }
        function onCompressionRequested() { compressDialog.open() }
        function onCredentialsRequested() { unlockDialog.open() }
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
        Material.background: App.colour.panel
        // Nothing in the toolbar is a keyboard destination; leaving these
        // focusable is how the keyboard ends up somewhere it can do nothing.
        focusPolicy: Qt.NoFocus

        RowLayout {
            id: headerRow
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
                font.pixelSize: App.headingSize
            }

            Item { Layout.fillWidth: true }

            Item { Layout.fillWidth: true }

            Label {
                visible: App.tasks.activeCount > 0
                text: App.tasks.activeCount + " running"
                color: App.colour.textMuted
            }

            BusyIndicator {
                running: App.tasks.activeCount > 0
                visible: running
                implicitWidth: 22
                implicitHeight: 22
            }
        }

            // Looks like the box it opens, sits where a browser would put a search
            // bar, and says which key does it. Its job is to be seen: the palette
            // is the answer to "how do I do X" and nobody finds a shortcut they were
            // never told about.
        Rectangle {
            objectName: "commandBar"
            // Anchored to the toolbar rather than laid out in the row: between the
            // other items is not the same as in the middle of the window, and the
            // middle of the window is where the eye goes looking for it.
            anchors.centerIn: parent
            width: Math.min(420, Math.max(240, root.width * 0.3))
            height: App.minimumTarget
            radius: 4
            color: barHover.containsMouse ? App.colour.hover : App.colour.panel
            border.color: App.colour.border
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 6
                spacing: 6

                Label {
                    text: "⌕"
                    color: App.colour.textMuted
                    font.pixelSize: App.textSize
                }
                Label {
                    Layout.fillWidth: true
                    text: "Search commands"
                    color: App.colour.textMuted
                    font.pixelSize: App.secondaryTextSize
                    elide: Text.ElideRight
                }
                // The key itself, drawn like a key. Reading it here once is how
                // someone stops needing this bar.
                Rectangle {
                    implicitWidth: shortcutLabel.implicitWidth + 10
                    implicitHeight: shortcutLabel.implicitHeight + 4
                    radius: 3
                    color: App.colour.hover
                    border.color: App.colour.border
                    border.width: 1

                    Label {
                        id: shortcutLabel
                        anchors.centerIn: parent
                        text: "Ctrl+R"
                        color: App.colour.textMuted
                        font.family: App.monospaceFont
                        font.pixelSize: App.smallTextSize
                    }
                }
            }

            MouseArea {
                id: barHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // Opens the real thing rather than trying to be it: one box with
                // the list, one place that owns the filtering.
                onClicked: commandPalette.open()
            }
            }

    }

    AppMenu { id: appMenu }
    CommandPalette { id: commandPalette }
    CompressDialog { id: compressDialog }

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
            return root.currentTabItem()
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

        // The divider, spelled out rather than left to the style. Material's own
        // takes its colour from `Material.background`, which the window no longer
        // sets, so it would have come out in Material's grey instead of the
        // window's -- the same shape and the same two states, from the palette.
        // The grip inside it stays Material's: it is the affordance the style
        // draws on every SplitView rather than a colour anybody chose, and it
        // follows the polarity on its own. See ADR-0074.
        handle: Rectangle {
            implicitWidth: 6
            implicitHeight: 6
            color: SplitHandle.pressed
                   ? App.colour.window
                   : App.colour.divider

            Rectangle {
                color: Material.secondaryTextColor
                width: parent.SplitHandle.pressed ? 3 : 1
                height: parent.SplitHandle.pressed ? 3 : 8
                radius: width
                x: (parent.width - width) / 2
                y: (parent.height - height) / 2

                Behavior on height { NumberAnimation { duration: 100 } }
            }
        }

        Sidebar {
            onFocusWanted: root.focusCurrentTab()
            SplitView.preferredWidth: 240
            SplitView.minimumWidth: 160
        }

        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0

            TabBar {
                id: tabBar
                Layout.fillWidth: true
                Material.background: App.colour.panel
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
                    delegate: ColumnLayout {
                        id: tabBody
                        required property int index
                        required property url viewSource
                        required property var controller
                        required property string featureId

                        spacing: 0

                        // What the shell talks to: this tab's loaded view.
                        property alias view: tabLoader.item

                        Loader {
                            id: tabLoader
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            asynchronous: false
                            source: tabBody.viewSource
                            onLoaded: {
                                if (item)
                                    item.controller = tabBody.controller
                            }
                        }

                        // Keep the keyboard with the tab the user is looking at.
                        onVisibleChanged: if (visible && tabLoader.item && tabLoader.item.focusActivePane)
                                              Qt.callLater(tabLoader.item.focusActivePane)
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
                    color: App.colour.textMuted
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    Layout.maximumWidth: 420
                    color: App.colour.textFaint
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
                    color: App.colour.textFaint
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
                color: App.colour.border
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
                color: App.colour.border
            }

            TaskStrip {
                // Named so a test can ask where it starts: it is what the search
                // form's criteria used to disappear behind. See MOLE-272.
                objectName: "taskStrip"
                Layout.fillWidth: true
            }
        }
    }
    }

    MoleDialog {
        id: aboutDialog
        objectName: "aboutDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        title: "Loaded plugins"
        preferredWidth: 560

        footer: ConfirmButtons { dismissOnly: true }

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
                color: App.colour.border
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: App.colour.textMuted
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
                    color: App.colour.bad
                    text: modelData
                }
            }
        }
    }

    MoleDialog {
        id: shortcutDialog
        objectName: "shortcutDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        title: "Keyboard"
        preferredWidth: 620

        footer: ConfirmButtons { dismissOnly: true }

        readonly property var groups: [
            { "heading": "Tabs", "keys": [
                ["Ctrl+T", "New browser tab"],
                ["Ctrl+Shift+T", "New dual-pane tab"],
                ["just start typing", "Filter this folder"],
                ["Ctrl+F", "Search this folder (new tab)"],
                ["Ctrl+Shift+I", "Search everywhere indexed (new tab)"],
                ["F3", "Preview the file under the cursor"],
                ["Space / Backspace", "Next / previous file, in a preview"],
                ["Ctrl+Shift+A", "Analyse the selected folders, or this one"],
                ["Ctrl+R", "Find any command, bookmark or drive"],
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
                ["F8 or Delete", "Delete"],
                ["Ctrl+Shift+C", "Copy this folder's path"],
                ["Ctrl+Shift+F", "Copy the selected file's path"]]}
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
                        color: App.colour.accent
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
                                font.family: App.monospaceFont
                                font.pixelSize: 12
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData[1]
                                color: App.colour.textMuted
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
            if (actionId === "mole.view.findInPreview") {
                var previewing = root.currentTabItem()
                if (previewing && previewing.findInViewer)
                    previewing.findInViewer()
            }
            else if (actionId === "mole.view.filter") {
                var view = root.currentTabItem()
                if (view && view.focusFilter)
                    view.focusFilter()
            }
            else if (actionId === "mole.help.shortcuts")
                shortcutDialog.open()
            else if (actionId === "mole.help.plugins" || actionId === "mole.help.about")
                aboutDialog.open()
        }
        function onIndexFolderRequested(uri, label) {
            // The search tab has only just been opened, so its body is a Loader
            // that has not instantiated its view yet. Asked for on the next turn
            // of the loop, when there is something to ask.
            Qt.callLater(function() {
                var view = root.currentTabItem()
                if (view && view.openIndexDialog)
                    view.openIndexDialog(uri, label)
            })
        }
        function onNotification(severity, title, detail) {
            notificationPopup.offersRelease = false
            notificationPopup.show(detail.length > 0 ? title + " — " + detail : title)
        }
        // A newer Mole exists. The same toast, and the version is all it says: the
        // release page has the notes, and a notice that carries them is a notice
        // that needs scrolling. Shown once per version -- UpdateCheck decides when,
        // and this appearing is what counts as having said it, whether the button
        // is pressed, the toast dismissed or the window closed. See MOLE-325.
        function onVersionAvailable(version) {
            notificationPopup.offersRelease = true
            notificationPopup.show("Mole " + version + " has been released.")
        }
    }

    // The window's own toast, and the component the browser view uses too: one
    // popup, one timer, one closePolicy. See ui/Toast.qml and MOLE-398.
    Toast {
        id: notificationPopup
        objectName: "notificationPopup"
        liftBy: 80

        /// Whether this notice has somewhere to go, which is the update notice and
        /// nothing else so far.
        property bool offersRelease: false

        // Not while there is a button. Five seconds is right for something to
        // read and wrong for something to press: a link that disappears while
        // somebody reaches for it is worse than no link. This notice goes when
        // it is dismissed, which is a click anywhere outside it.
        dwellMs: offersRelease ? 0 : 5000

        ActionButton {
            objectName: "openReleasePageButton"
            visible: notificationPopup.offersRelease
            // Outlined rather than filled: this is an offer, not the one thing
            // the window exists to do.
            filled: false
            text: "Release page"
            onClicked: {
                // The application opens what the manifest handed it. Nothing
                // here knows the URL, which is why there is nothing here that
                // could assemble one.
                App.openReleasePage()
                notificationPopup.close()
            }
        }
    }
}
