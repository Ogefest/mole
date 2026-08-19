import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One navigable pane. Used once for a single view and twice for a dual view.
//
// All keyboard handling lives here rather than in the window, because a key
// only means something relative to the pane that has focus.
FocusScope {
    id: pane

    property var paneController: null
    property bool active: true
    /// Tiles instead of rows. Both views share the model, the cursor and every
    /// key handler -- only the delegate and the layout differ.
    property bool gridMode: false
    signal focusRequested()
    signal transferRequested(bool move)
    signal switchPaneRequested()
    /// A drop that would overwrite something. The pane does not own the
    /// confirmation -- the view does, and it is the same one F5 uses.
    signal dropNeedsAnswer(var urls, var plan)

    // A FocusScope, not a plain Rectangle, and the key handling lives here
    // rather than on the list.
    //
    // Whichever control the window manager decides to focus when the window
    // opens -- in practice the path field, being the first focusable one --
    // the arrows have to keep driving the list. Keys the focused child really
    // wants (letters, Space, Enter in the path bar) are consumed there and
    // never reach this handler, so both behaviours hold at once.
    // Qt gives several of these their own handler, and those are the ones that
    // were demonstrably working while everything routed through onPressed was
    // not. Anything with a dedicated handler now uses it; onPressed is left
    // with the keys Qt has no handler for.
    // Ctrl + arrows mirror the three toolbar buttons: back, forward, up.
    // Without a modifier the arrows drive the cursor.
    Keys.onUpPressed: function(event) {
        if (!paneController || editingPath)
            return
        if (event.modifiers & Qt.ControlModifier)
            paneController.goUp()
        else
            paneController.moveCursor(-1)
    }
    Keys.onDownPressed: function(event) {
        if (!paneController || editingPath)
            return
        paneController.moveCursor(1)
    }
    Keys.onLeftPressed: function(event) {
        if (!paneController || editingPath)
            return
        if (event.modifiers & Qt.ControlModifier)
            paneController.goBack()
    }
    Keys.onRightPressed: function(event) {
        if (!paneController || editingPath)
            return
        if (event.modifiers & Qt.ControlModifier)
            paneController.goForward()
    }
    Keys.onReturnPressed: {
        pane.trace("Return", "Return")
        if (paneController && !editingPath)
            pane.openRow(paneController.currentIndex)
    }
    Keys.onEnterPressed: {
        pane.trace("Enter", "Enter")
        if (paneController && !editingPath)
            pane.openRow(paneController.currentIndex)
    }
    Keys.onEscapePressed: if (paneController && !editingPath) paneController.files.clearSelection()
    Keys.onSpacePressed: {
        if (paneController && !editingPath)
            paneController.files.toggleSelected(paneController.currentIndex)
    }
    Keys.onAsteriskPressed: if (paneController && !editingPath) paneController.files.invertSelection()
    Keys.onTabPressed: if (!editingPath) pane.switchPaneRequested()
    Keys.onDeletePressed: {
        if (paneController && !editingPath && paneController.targetCount() > 0)
            deleteDialog.open()
    }

    Keys.onPressed: function(event) {
        pane.trace("onPressed", event.key)
        if (!paneController || editingPath)
            return

        // Typing a printable character starts filtering, the way a file
        // manager has always behaved. No shortcut to remember: the first
        // keystroke opens the bar and goes into it.
        //
        // Guarded on the modifiers, or Ctrl+D would type a "d" into the filter
        // instead of adding a bookmark.
        if (event.text.length > 0 && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                                          | Qt.MetaModifier))) {
            var ch = event.text
            if (ch >= " " && ch !== "\u007f") {
                pane.beginFilter(ch)
                event.accepted = true
                return
            }
        }

        switch (event.key) {
        case Qt.Key_Backspace:
            paneController.goUp()
            event.accepted = true
            break
        case Qt.Key_Home:
            paneController.cursorToStart()
            event.accepted = true
            break
        case Qt.Key_End:
            paneController.cursorToEnd()
            event.accepted = true
            break
        case Qt.Key_PageUp:
            paneController.moveCursor(-15)
            event.accepted = true
            break
        case Qt.Key_PageDown:
            paneController.moveCursor(15)
            event.accepted = true
            break
        case Qt.Key_Insert:
            paneController.toggleSelectionAndAdvance()
            event.accepted = true
            break
        case Qt.Key_A:
            if (event.modifiers & Qt.ControlModifier) {
                paneController.files.selectAll()
                event.accepted = true
            }
            break
        // --- commander function keys ---
        case Qt.Key_F3:
            // Handled here as well as by the window shortcut: the pane is
            // where the file under the cursor is known, and a key that only
            // works when a shortcut happens to fire is a key that will one day
            // stop working.
            //
            // A folder has nothing to preview, and a key that does nothing is
            // indistinguishable from one that is broken -- so on a folder F3
            // opens it, which is the same thing Return does. The menu entry
            // stays literally "Preview this file" and stays disabled here,
            // because that is what it says it does.
            if (paneController && paneController.files.isDirAt(paneController.currentIndex))
                pane.openRow(paneController.currentIndex)
            else
                App.triggerAction("mole.tools.preview")
            event.accepted = true
            break
        case Qt.Key_F2:
            renameDialog.open()
            event.accepted = true
            break
        case Qt.Key_F5:
            pane.transferRequested(false)
            event.accepted = true
            break
        case Qt.Key_F6:
            pane.transferRequested(true)
            event.accepted = true
            break
        case Qt.Key_F7:
            mkdirDialog.open()
            event.accepted = true
            break
        case Qt.Key_F8:
            if (paneController.targetCount() > 0)
                deleteDialog.open()
            event.accepted = true
            break
        }
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        color: "#151922"
        border.color: pane.active ? Material.accent : "#2a3140"
        border.width: 1
        opacity: pane.active ? 1.0 : 0.85
    }

    // The cursor lives in the controller and nowhere else.
    //
    // It used to live in both: the ListView bound its currentIndex to the
    // controller, and also wrote back whenever it changed. Assigning to
    // currentIndex breaks a QML binding permanently, so one click was enough
    // to sever the link -- after which a model reset (any navigation) moved
    // the view's index without the controller hearing about it. Enter then
    // opened whatever had been selected before.
    //
    // Now QML only ever reads. Every write goes to the controller and the
    // binding follows.

    // --debug-keys reports where each keystroke went. Focus behaviour depends
    // on the window manager, which makes it the one thing that cannot be
    // reproduced from a test.
    // While the path bar has the keyboard, its keys are its own. Return there
    // means "go to what I typed", not "open the row under the cursor".
    readonly property bool editingPath: pathField.activeFocus || filterField.activeFocus

    readonly property bool debugKeys: Qt.application.arguments.indexOf("--debug-keys") >= 0
    function trace(where, key) {
        if (debugKeys)
            console.log("[pane]", where, "key=" + key,
                        "cursor=" + (paneController ? paneController.currentIndex : -1))
    }

    function focusPathBar() {
        pathField.forceActiveFocus()
        pathField.selectAll()
    }

    // The filter belongs to one folder; leaving it takes the bar with it.
    //
    // And the text, not only the bar. Hiding the bar alone left the new folder
    // silently filtered by the old term -- a listing missing most of its files
    // with nothing on screen to explain why.
    Connections {
        target: paneController
        function onLocationChanged() {
            filterRow.visible = false
            filterField.text = ""
            if (paneController)
                paneController.files.filterText = ""
        }
    }

    function focusFilter() {
        filterRow.visible = true
        filterField.forceActiveFocus()
        filterField.selectAll()
    }

    /// Opens the filter bar already containing the keystroke that opened it,
    /// so nothing the user typed is lost.
    function beginFilter(character) {
        filterRow.visible = true
        filterField.text = character
        filterField.forceActiveFocus()
        filterField.cursorPosition = filterField.text.length
    }

    function clearFilter() {
        if (paneController)
            paneController.files.filterText = ""
        filterRow.visible = false
        pane.takeFocus()
    }

    function takeFocus() {
        pane.focusRequested()
        pane.forceActiveFocus()
        if (gridMode)
            grid.forceActiveFocus()
        else
            list.forceActiveFocus()
    }

    // A press that has become a drag, on `row`.
    //
    // What goes is what the controller says goes: the ticked rows when `row` is
    // one of them, that row alone when it is not. Nothing here ticks anything and
    // nothing here moves the cursor -- dragging is not selecting, and somebody who
    // drags one file out of a ticked ten has to find the ten still ticked
    // afterwards.
    function beginDrag(row) {
        if (!paneController || row < 0)
            return
        App.startDrag(paneController.dragTargets(row))
    }

    // Directories open in place, a file that some plugin can mount becomes a
    // drive of its own, anything else goes to the desktop's default handler.
    function openRow(index) {
        if (debugKeys) {
            console.log("[pane] openRow", index,
                        paneController ? paneController.files.uriAt(index) : "<none>",
                        "isDir=" + (paneController ? paneController.files.isDirAt(index) : "?"))
        }
        if (!paneController || index < 0)
            return
        var uri = paneController.files.uriAt(index)
        if (uri.length === 0)
            return

        if (paneController.files.isDirAt(index))
            paneController.navigateTo(uri)
        else if (App.isMountableArchive(uri))
            App.openArchive(uri)
        else
            App.openExternally(uri)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            ToolButton {
                text: "←"
                enabled: paneController && paneController.canGoBack
                onClicked: { paneController.goBack(); pane.takeFocus() }
            }
            ToolButton {
                text: "→"
                enabled: paneController && paneController.canGoForward
                onClicked: { paneController.goForward(); pane.takeFocus() }
            }
            ToolButton {
                text: "↑"
                enabled: paneController && paneController.canGoUp
                onClicked: { paneController.goUp(); pane.takeFocus() }
            }
            ToolButton {
                text: "⟳"
                enabled: paneController !== null
                onClicked: { paneController.refresh(); pane.takeFocus() }
            }

            // Two views of one thing: crumbs to click, and a field to type in.
            //
            // Clicking a crumb beats pressing Backspace once per level, which is
            // what the alternative amounted to. Typing is still there -- Ctrl+G,
            // or a click on the empty space to the right -- because a path
            // pasted from somewhere else has to go somewhere.
            Item {
                Layout.fillWidth: true
                // Tall enough for whichever child needs the most room. A
                // hard-coded 30 squeezed the text field, which wants 40, so
                // Ctrl+G revealed a field with its text and underline clipped
                // -- it read as being covered by something.
                //
                // Measured from the field rather than switched per mode, or the
                // whole bar would change height as the keyboard moved into it.
                implicitHeight: Math.max(30, pathField.implicitHeight)

                property bool editing: pathField.activeFocus

                MouseArea {
                    anchors.fill: parent
                    // Only the gap after the last crumb: a click on a crumb is
                    // the crumb's own.
                    onClicked: pane.focusPathBar()
                }

                Flickable {
                    id: crumbFlick
                    anchors.fill: parent
                    visible: !parent.editing
                    contentWidth: crumbs.width
                    contentHeight: height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    // A long path scrolls to keep the end -- where you are --
                    // in view, rather than the start, which you already know.
                    onContentWidthChanged: contentX = Math.max(0, contentWidth - width)

                    Row {
                        id: crumbs
                        objectName: "pathCrumbs"
                        height: crumbFlick.height
                        spacing: 0

                        Repeater {
                            model: paneController ? paneController.pathSegments : []
                            delegate: Row {
                                required property var modelData
                                required property int index
                                height: crumbs.height

                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: index > 0
                                    text: "  ›  "
                                    color: "#4a5364"
                                    font.pixelSize: App.textSize
                                }

                                Rectangle {
                                    height: 22
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: crumbLabel.implicitWidth + 12
                                    radius: 3
                                    color: crumbMouse.containsMouse && !modelData.current
                                           ? "#26303f" : "transparent"

                                    Label {
                                        id: crumbLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: modelData.current ? "#e6ebf5" : "#9aa4b8"
                                        font.pixelSize: App.textSize
                                        font.bold: modelData.current
                                    }

                                    MouseArea {
                                        id: crumbMouse
                                        anchors.fill: parent
                                        hoverEnabled: !modelData.current
                                        enabled: !modelData.current
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            paneController.navigateTo(modelData.uri)
                                            pane.takeFocus()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                TextField {
                    id: pathField
                    objectName: "pathField"
                    anchors.fill: parent
                    visible: parent.editing
                    text: paneController ? paneController.displayPath : ""
                    selectByMouse: true
                    font.pixelSize: App.textSize
                    // Handled here rather than through onAccepted, and accepted
                    // explicitly. onAccepted moves focus back to the list first,
                    // and the un-accepted key then bubbles to the pane -- which
                    // saw the path bar no longer focused and opened a row nobody
                    // asked for.
                    Keys.onReturnPressed: function(event) {
                        pathField.goToTypedPath()
                        event.accepted = true
                    }
                    Keys.onEnterPressed: function(event) {
                        pathField.goToTypedPath()
                        event.accepted = true
                    }

                    function goToTypedPath() {
                        if (!paneController)
                            return
                        // Typed paths are a convenience; anything with a scheme
                        // is passed through so remote drives stay reachable.
                        var value = text.trim()
                        if (value.indexOf("://") < 0)
                            value = "file://" + value
                        paneController.navigateTo(value)
                        pane.takeFocus()
                    }
                    Keys.onEscapePressed: pane.takeFocus()
                }
            }

            BusyIndicator {
                running: paneController ? paneController.loading : false
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }
        }

        // Filtering is not searching: it hides rows that are already loaded and
        // never leaves this folder, which is why it is a bar here rather than
        // a tab of its own.
        RowLayout {
            id: filterRow
            Layout.fillWidth: true
            visible: false
            spacing: 6

            Label {
                text: "Filter"
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
            }

            TextField {
                id: filterField
                objectName: "filterField"
                Layout.fillWidth: true
                placeholderText: "part of a name"
                font.pixelSize: App.secondaryTextSize
                onTextChanged: {
                    if (paneController)
                        paneController.files.filterText = text
                }
                Keys.onEscapePressed: function(event) {
                    pane.clearFilter()
                    event.accepted = true
                }
                // Backspacing past the start puts the keyboard back on the
                // list rather than leaving an empty bar in the way.
                // Filtering to two rows and pressing Enter means "open that
                // one", not "now start using the list". Moving the keyboard
                // first costs a keystroke and loses the one the user just
                // spent -- and typing more still has to narrow the filter, so
                // the field keeps the keyboard throughout.
                Keys.onReturnPressed: function(event) {
                    if (paneController)
                        pane.openRow(paneController.currentIndex)
                    event.accepted = true
                }
                Keys.onDownPressed: function(event) {
                    if (paneController) paneController.moveCursor(1)
                    event.accepted = true
                }
                Keys.onUpPressed: function(event) {
                    if (paneController) paneController.moveCursor(-1)
                    event.accepted = true
                }
                // Paging works from here too, for a filter that still leaves
                // more rows than fit.
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Backspace && filterField.text.length === 0) {
                        pane.clearFilter()
                        event.accepted = true
                    } else if (event.key === Qt.Key_PageDown) {
                        if (paneController) paneController.moveCursor(10)
                        event.accepted = true
                    } else if (event.key === Qt.Key_PageUp) {
                        if (paneController) paneController.moveCursor(-10)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Tab) {
                        // An explicit way out, for when the list is where the
                        // user wants to be.
                        pane.takeFocus()
                        event.accepted = true
                    }
                }
            }

            Label {
                visible: paneController && paneController.files.filterText.length > 0
                text: paneController
                      ? paneController.files.count + " of " + paneController.files.totalCount
                      : ""
                color: Material.accent
                font.pixelSize: App.secondaryTextSize
            }

            ToolButton {
                text: "×"
                font.pixelSize: App.textSize
                implicitWidth: 24
                implicitHeight: 24
                onClicked: pane.clearFilter()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: paneController && paneController.errorText.length > 0
            text: paneController ? paneController.errorText : ""
            color: Material.color(Material.Red)
            wrapMode: Text.Wrap
            font.pixelSize: App.secondaryTextSize
        }

        // Above the listing, and only when the folder in view is inside a
        // checkout. See ADR-0041 for why it is read-only and local drives only.
        RepositoryBand {
            info: paneController ? paneController.repository : null
            // A changed path goes where anything else in Mole goes -- except a
            // deleted one, which has nowhere to go, so it answers with the folder
            // that held it and no cursor. See ADR-0042.
            onPathActivated: function (uri, deleted) {
                if (!paneController)
                    return
                if (deleted)
                    paneController.revealMissingFile(uri)
                else
                    paneController.revealFile(uri)
            }
        }

        // Scanning 50 000 files takes seconds, and an empty pane during those
        // seconds reads as "this folder is empty" rather than "still working".
        // One second is the threshold: below it a spinner is just a flash.
        Timer {
            id: slowListing
            interval: 1000
            repeat: false
            property bool tripped: false
            onTriggered: tripped = true
        }

        Connections {
            target: paneController
            function onLoadingChanged() {
                if (paneController.loading) {
                    slowListing.tripped = false
                    slowListing.restart()
                } else {
                    slowListing.stop()
                    slowListing.tripped = false
                }
            }
        }

        // Anchored inside a filling Item rather than laid out. A ColumnLayout
        // is only as wide as its widest child, so centring within it put this
        // against the left edge of the pane instead of in the middle of it.
        Item {
            objectName: "loadingView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: slowListing.tripped && paneController && paneController.loading

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 420)
                spacing: 10

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: parent.parent.visible
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Reading " + (paneController ? paneController.locationName : "") + "…"
                    font.pixelSize: App.textSize
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: "A folder with tens of thousands of files takes a moment. Nothing is stuck."
                    color: "#6f7788"
                    font.pixelSize: App.smallTextSize
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Stop"
                    flat: true
                    font.pixelSize: App.smallTextSize
                    focusPolicy: Qt.NoFocus
                    onClicked: if (paneController) paneController.goBack()
                }
            }
        }

        ListView {
            id: list
            objectName: "fileList"
            visible: !pane.gridMode && !(slowListing.tripped && paneController && paneController.loading)

            Layout.fillWidth: true
            Layout.fillHeight: pane.gridMode ? false : true
            Layout.preferredHeight: pane.gridMode ? 0 : -1
            clip: true
            model: paneController ? paneController.files : null
            currentIndex: paneController ? paneController.currentIndex : -1
            // Default focus target inside the scope, so focusing the pane puts
            // the cursor on the list rather than on the path bar.
            focus: true

            // The view must not move its own cursor. Its built-in arrow-key
            // handling assigns currentIndex directly, which severs the binding
            // to the controller -- after which the two drift apart and Enter
            // acts on whichever row the controller still thinks is current.
            // One owner, one writer.
            keyNavigationEnabled: false
            // Light, reused delegates are what hold a 10k-row listing at a
            // smooth scroll.
            reuseItems: true
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            onActiveFocusChanged: if (activeFocus) pane.focusRequested()

            delegate: ItemDelegate {
                required property int index
                required property string name
                required property bool isDir
                required property string sizeText
                required property string modifiedText
                required property string iconText
                required property bool selected
                required property bool hasReport
                required property bool hasAlert
                required property bool alertTriggered
                required property string gitMark

                width: ListView.view.width
                // From the scale, so raising the text size cannot crop a row.
                height: App.listRowHeight
                highlighted: ListView.isCurrentItem

                // A row is a click target, never a keyboard destination -- but
                // ListView hands focus to its current delegate by design, and
                // no focus policy prevents that. So the row is made transparent
                // instead: keys go to the pane first.
                //
                // This is the whole Enter bug. ItemDelegate is a button
                // underneath, and a focused button swallows Return, which is
                // why every other key worked and that one did not.
                focusPolicy: Qt.NoFocus
                Keys.forwardTo: [pane]

                // Flat and instant. The Material style fades its hover in over
                // roughly 200ms, which makes a file list feel like it is
                // lagging behind the pointer.
                background: Rectangle {
                    // No hover highlight. The cursor row is where Enter will
                    // act; a second highlight trailing the pointer made it
                    // ambiguous which of the two that was.
                    color: highlighted ? "#2b3547" : "transparent"
                }

                onClicked: {
                    pane.takeFocus()
                    // Written to the controller, never to the view: assigning
                    // list.currentIndex here is what used to break the binding.
                    paneController.currentIndex = index
                }
                onDoubleClicked: pane.openRow(index)

                // `target: null`, so the row is not dragged out of the layout:
                // this handler exists to report that a press has become a drag,
                // not to move anything. Qt's own threshold decides when that
                // happened, and until it does the delegate keeps the click and
                // the double click it already had -- both load-bearing, and both
                // easy to break from on top.
                DragHandler {
                    objectName: "rowDragHandler"
                    target: null
                    onActiveChanged: if (active) pane.beginDrag(index)
                }

                // Ticked rows stay obvious even when the cursor is elsewhere.
                Rectangle {
                    anchors.fill: parent
                    visible: selected
                    color: Material.accent
                    opacity: 0.18
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    Label { text: iconText; font.pixelSize: App.textSize }

                    Label {
                        objectName: "fileName"
                        Layout.fillWidth: true
                        text: name
                        elide: Text.ElideMiddle
                        font.pixelSize: App.textSize
                        font.bold: selected
                        color: selected ? Material.accent : Material.foreground
                    }

                    Label {
                        Layout.preferredWidth: 80
                        horizontalAlignment: Text.AlignRight
                        text: sizeText
                        color: "#8b93a7"
                        font.pixelSize: App.secondaryTextSize
                    }

                    // What the application already knows about this row.
                    // Visible at a glance beats having to open each folder to
                    // find out whether it is being watched or reported on.
                    Row {
                        spacing: 4
                        Rectangle {
                            visible: hasReport
                            width: 46
                            height: 15
                            radius: 2
                            color: "transparent"
                            border.color: "#3f5f80"
                            Label {
                                anchors.centerIn: parent
                                text: "report"
                                color: "#7cc4ff"
                                font.pixelSize: App.smallTextSize
                            }
                        }
                        Rectangle {
                            visible: hasAlert
                            width: 40
                            height: 15
                            radius: 2
                            color: "transparent"
                            border.color: alertTriggered ? "#e5534b" : "#4a5364"
                            Label {
                                anchors.centerIn: parent
                                text: "alert"
                                color: alertTriggered ? "#e5534b" : "#8b93a7"
                                font.pixelSize: App.smallTextSize
                            }
                        }

                        // What git says about this row. git's own letter, which is
                        // the signal -- the colour only agrees with it, because a
                        // mark that means something in colour alone means nothing
                        // to a reader who cannot separate two of them (ADR-0010).
                        //
                        // Invisible rather than empty when there is nothing to say,
                        // so a folder in no checkout has no column here at all.
                        GitMark {
                            objectName: "gitMarker"
                            visible: gitMark.length > 0
                            width: visible ? 18 : 0
                            mark: gitMark
                        }
                    }

                    Label {
                        Layout.preferredWidth: 130
                        horizontalAlignment: Text.AlignRight
                        text: modifiedText
                        color: "#8b93a7"
                        font.pixelSize: App.secondaryTextSize
                    }
                }
            }
        }

        GridView {
            id: grid
            objectName: "fileGrid"
            visible: pane.gridMode

            Layout.fillWidth: true
            Layout.fillHeight: pane.gridMode
            Layout.preferredHeight: pane.gridMode ? -1 : 0
            clip: true
            model: paneController ? paneController.files : null
            currentIndex: paneController ? paneController.currentIndex : -1
            focus: pane.gridMode
            // Same reason as the list: one owner for the cursor.
            keyNavigationEnabled: false
            cellWidth: 132
            cellHeight: 104
            ScrollBar.vertical: ScrollBar {}

            onActiveFocusChanged: if (activeFocus) pane.focusRequested()

            delegate: ItemDelegate {
                required property int index
                required property string name
                required property bool isDir
                required property string sizeText
                required property string iconText
                required property bool selected

                width: grid.cellWidth - 8
                height: grid.cellHeight - 8
                highlighted: GridView.isCurrentItem
                focusPolicy: Qt.NoFocus
                Keys.forwardTo: [pane]

                background: Rectangle {
                    radius: 4
                    // No hover highlight. The cursor row is where Enter will
                    // act; a second highlight trailing the pointer made it
                    // ambiguous which of the two that was.
                    color: highlighted ? "#2b3547" : "transparent"
                    border.color: selected ? Material.accent : "transparent"
                    border.width: selected ? 1 : 0
                }

                onClicked: {
                    pane.takeFocus()
                    paneController.currentIndex = index
                }
                onDoubleClicked: pane.openRow(index)

                // The same handler as the list delegate's, for the same reason:
                // grid is a way of looking at this pane rather than a different
                // pane, so every gesture has to work in both.
                DragHandler {
                    objectName: "tileDragHandler"
                    target: null
                    onActiveChanged: if (active) pane.beginDrag(index)
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 2

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: iconText
                        font.pixelSize: 34
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: name
                        elide: Text.ElideMiddle
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                        font.pixelSize: App.smallTextSize
                        font.bold: selected
                        color: selected ? Material.accent : Material.foreground
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: sizeText
                        color: "#6f7788"
                        font.pixelSize: App.smallTextSize
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: {
                    if (!paneController || !paneController.files)
                        return ""
                    var files = paneController.files
                    return files.selectionCount > 0
                        ? files.selectionCount + " of " + files.count + " selected"
                        : files.count + " items"
                }
                color: paneController && paneController.files
                       && paneController.files.selectionCount > 0
                       ? Material.accent : "#8b93a7"
                font.pixelSize: App.smallTextSize
            }

            Label {
                visible: paneController && !paneController.writable
                text: "read-only"
                color: "#8b93a7"
                font.pixelSize: App.smallTextSize
            }

            Item { Layout.fillWidth: true }
        }
    }

    // --- taking a drop -------------------------------------------------
    //
    // One area over the whole pane rather than one per view: a pane is one place
    // to put something, whether its rows are drawn as lines or as tiles. Last in
    // the file so it sits above them both, and it takes no pointer events -- a
    // click still belongs to the row underneath.
    DropArea {
        id: dropTarget
        objectName: "paneDropArea"
        anchors.fill: parent
        keys: ["text/uri-list"]

        // A read-only pane takes no part at all. Refusing while the pointer is
        // still moving is honest -- the desktop shows it cannot be dropped here --
        // and refusing after the button has been released is a failure message
        // about something the user has already committed to. The pane says
        // "read-only" in its status line throughout, which is the sentence that
        // explains the cursor.
        enabled: paneController !== null && paneController.writable

        /// What the drop would do, read on entry and shown while it is over us.
        property var plan: ({})

        function addressesIn(event) {
            var out = []
            for (var i = 0; i < event.urls.length; ++i)
                out.push(String(event.urls[i]))
            return out
        }

        onEntered: function(drag) {
            // A drag that started in this window is not a drop. Pane to pane is
            // F5 and F6, and taking it here would mean a drag onto the folder it
            // came from asking the user about collisions with itself.
            if (!paneController || drag.source !== null) {
                drag.accepted = false
                return
            }
            dropTarget.plan = paneController.dropPlan(dropTarget.addressesIn(drag))
        }

        onExited: dropTarget.plan = ({})

        onDropped: function(drop) {
            dropTarget.plan = ({})
            if (!paneController)
                return

            var urls = dropTarget.addressesIn(drop)
            var plan = paneController.dropPlan(urls)

            // The files are here now, so this is where the user is.
            pane.takeFocus()

            // A copy, and said so explicitly. Accepting the *proposed* action
            // would tell a source that offered a move that its file may be
            // deleted -- see ADR-0040.
            drop.accept(Qt.CopyAction)

            if ((plan.collisions || []).length > 0) {
                pane.dropNeedsAnswer(urls, plan)
                return
            }
            paneController.dropHere(urls, "stop")
        }

        // What would happen, while it can still change what the user does. In the
        // pane's own palette rather than a system tooltip: it is a statement about
        // this folder.
        Rectangle {
            objectName: "dropHint"
            visible: dropTarget.containsDrag && (dropTarget.plan.count || 0) > 0
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 34
            radius: 4
            color: "#1b2029"
            border.color: Material.accent
            border.width: 1
            implicitWidth: dropHintText.implicitWidth + 24
            implicitHeight: dropHintText.implicitHeight + 16

            Label {
                id: dropHintText
                objectName: "dropHintText"
                anchors.centerIn: parent
                font.pixelSize: App.secondaryTextSize
                color: "#e6ebf5"
                text: {
                    var count = dropTarget.plan.count || 0
                    var size = dropTarget.plan.sizeText || ""
                    var where = dropTarget.plan.targetPath || ""
                    var what = (count === 1 ? "1 item" : count + " items")
                    return "Copy " + what + (size.length > 0 ? " · " + size : "") + " → " + where
                }
            }
        }
    }

    // --- dialogs -------------------------------------------------------

    Dialog {
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: mkdirDialog
        title: "New folder"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 400
        footer: ConfirmButtons { acceptText: "Create" }

        onOpened: { mkdirField.text = ""; mkdirField.forceActiveFocus() }
        onAccepted: { paneController.createDirectory(mkdirField.text); pane.takeFocus() }
        onRejected: pane.takeFocus()

        TextField {
            id: mkdirField
            anchors.fill: parent
            placeholderText: "Folder name"
            selectByMouse: true
            onAccepted: mkdirDialog.accept()
        }
    }

    Dialog {
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: renameDialog
        title: "Rename"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 400
        footer: ConfirmButtons { acceptText: "Rename" }

        onOpened: {
            renameField.text = paneController ? paneController.currentName : ""
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        onAccepted: { paneController.renameCurrent(renameField.text); pane.takeFocus() }
        onRejected: pane.takeFocus()

        TextField {
            id: renameField
            anchors.fill: parent
            selectByMouse: true
            onAccepted: renameDialog.accept()
        }
    }

    Dialog {
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: deleteDialog
        objectName: "deleteDialog"
        title: "Delete"
        modal: true
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and forceActiveFocus() quietly does nothing.
        focus: true
        anchors.centerIn: Overlay.overlay
        width: 440
        footer: ConfirmButtons {
            acceptText: "Delete"
            rejectText: "Keep"
            destructive: true
        }

        // Taken once, when the question is asked. Binding it live would mean the
        // list could change under the dialog -- a refresh landing, a watcher
        // firing -- between reading it and pressing Yes, so what was agreed to
        // and what happens are the same rows by construction.
        property var doomed: []

        onAboutToShow: doomed = paneController ? paneController.targetDetails() : []
        onAccepted: { paneController.deleteTargets(); pane.takeFocus() }
        onRejected: pane.takeFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                objectName: "deleteQuestion"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: App.textSize
                text: "Permanently delete " + deleteDialog.doomed.length
                      + (deleteDialog.doomed.length === 1 ? " item?" : " items?")
            }
            // Which ones, not just how many. Deleting is the operation with no
            // second chance, so it is the one that must not be guessed at.
            TargetList {
                objectName: "deleteTargetList"
                Layout.fillWidth: true
                model: deleteDialog.doomed
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "A folder goes with everything inside it. This cannot be undone."
                color: "#d9a441"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
