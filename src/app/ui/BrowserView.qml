import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tab body for the browsing workflow. `controller` is injected by the shell.
//
// Single and dual pane are the same view in two modes, so switching between
// them keeps both panes' locations and history intact.
Item {
    id: view

    property var controller: null

    function focusActivePane() {
        if (!controller)
            return
        if (controller.activePaneIndex === 1 && controller.splitEnabled)
            rightPane.takeFocus()
        else
            leftPane.takeFocus()
    }

    // Called by the window-level fallback when a navigation key reached the
    // window without anything else claiming it.
    function activateCurrentRow() {
        var target = (controller && controller.activePaneIndex === 1 && controller.splitEnabled)
                     ? rightPane : leftPane
        target.openRow(target.paneController ? target.paneController.currentIndex : -1)
    }

    function moveCursorBy(delta) {
        if (controller && controller.activePane)
            controller.activePane.moveCursor(delta)
    }

    function goUpOneFolder() {
        if (controller && controller.activePane)
            controller.activePane.goUp()
    }

    function focusFilter() {
        var target = (controller && controller.activePaneIndex === 1 && controller.splitEnabled)
                     ? rightPane : leftPane
        target.focusFilter()
    }

    function focusPathBar() {
        var target = (controller && controller.activePaneIndex === 1 && controller.splitEnabled)
                     ? rightPane : leftPane
        target.focusPathBar()
    }

    // The tab was just shown -- put the keyboard somewhere useful.
    onVisibleChanged: if (visible) Qt.callLater(focusActivePane)
    Component.onCompleted: Qt.callLater(focusActivePane)

    function requestTransfer(move) {
        if (!controller)
            return
        if (!controller.splitEnabled) {
            transferHint.open()
            return
        }
        // A property now, not a method. Calling it threw, and the throw took the
        // rest of this function with it -- so F5 did nothing at all, silently.
        if (!controller.canTransfer)
            return
        transferDialog.prepare(move)
    }

    Connections {
        target: controller
        function onOperationFailed(message) {
            errorPopup.show(message)
        }
        // One treatment per kind of answer, and the same one whichever drive
        // produced it -- this view has no idea which did. See ADR-0075.
        function onDriveActionText(title, text, validUntilText) {
            driveActionResult.showText(title, text, validUntilText)
        }
        function onDriveActionUris(title, choices) {
            driveActionResult.showChoices(title, choices)
        }
    }

    DriveActionResult {
        id: driveActionResult
        // Opened through the pane that ran the action, so an earlier version of a
        // file goes wherever a file of that kind goes.
        openRequested: function(uri) {
            if (controller && controller.activePane)
                controller.activePane.openUri(uri)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6

                // Mode is a two-way switch, not a hidden toggle: which layout you
                // are in should be visible without trying it.
                //
                // The current one is a filled pill in bold, drawn here rather than
                // left to `Button { highlighted: true }`. Material paints a
                // highlighted label white whatever the theme and fills it from the
                // background the button inherited -- which is this toolbar, so on
                // a dark window the pill was the same colour as the bar behind it
                // and the only signal was the drop shadow, and on a light one it
                // was white on white. Two channels rather than one, and both from
                // the palette. See ADR-0074.
                RowLayout {
                    spacing: 0

                    component ModeButton: Button {
                        required property bool current
                        flat: true
                        font.pixelSize: 12
                        font.bold: current
                        Material.background: current ? App.colour.selection : "transparent"
                        Material.foreground: current ? App.colour.text : App.colour.textSecondary
                    }

                    ModeButton {
                        text: "▭  Single"
                        current: controller
                                 ? (!controller.splitEnabled && !controller.tilesEnabled) : false
                        onClicked: { controller.viewMode = 0; view.focusActivePane() }
                    }
                    ModeButton {
                        text: "◫  Dual"
                        current: controller ? controller.splitEnabled : false
                        onClicked: { controller.viewMode = 1; view.focusActivePane() }
                    }
                    ModeButton {
                        text: "▦  Grid"
                        current: controller ? controller.gridEnabled : false
                        onClicked: { controller.viewMode = 2; view.focusActivePane() }
                    }
                    ModeButton {
                        objectName: "galleryButton"
                        text: "▤  Gallery"
                        current: controller ? controller.galleryEnabled : false
                        onClicked: { controller.viewMode = 3; view.focusActivePane() }
                    }
                }

                ToolSeparator {}

                // Bound to a property, not to a method call: as an invokable
                // there was nothing to notify the binding, so these stayed
                // greyed out after switching to dual pane.
                ToolButton {
                    objectName: "copyButton"
                    enabled: controller ? controller.canTransfer : false
                    text: "F5  Copy"
                    font.pixelSize: 12
                    onClicked: view.requestTransfer(false)
                }
                ToolButton {
                    objectName: "moveButton"
                    enabled: controller ? controller.canTransfer : false
                    text: "F6  Move"
                    font.pixelSize: 12
                    onClicked: view.requestTransfer(true)
                }
                // Alongside copy and move, not in the status line at the
                // bottom: it changes what the pane shows, which is what the
                // rest of this strip is for.
                CheckBox {
                    objectName: "hiddenToggle"
                    text: "Hidden"
                    font.pixelSize: 12
                    focusPolicy: Qt.NoFocus
                    checked: controller && controller.activePane && controller.activePane.files
                             ? controller.activePane.files.showHidden : false
                    onToggled: {
                        // Both panes, so a dual view does not show two different
                        // ideas of what is in the same tree.
                        if (!controller)
                            return
                        if (controller.left && controller.left.files)
                            controller.left.files.showHidden = checked
                        if (controller.right && controller.right.files)
                            controller.right.files.showHidden = checked
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Show dotfiles and anything the drive marks hidden"
                    ToolTip.delay: 600
                }

                ToolButton {
                    visible: controller ? controller.splitEnabled : false
                    text: "⇄  Mirror"
                    font.pixelSize: 12
                    ToolTip.visible: hovered
                    ToolTip.text: "Point the other pane at this location"
                    onClicked: controller.mirrorToOtherPane()
                }

                Item { Layout.fillWidth: true }

                // What is already known about this folder. Indexing itself has
                // moved to the menu -- it is a once-in-a-while action, not a
                // working control, and the strip is for what you are doing now.
                Row {
                    spacing: 6

                    // A tag, not a button that looks like a tag: only the
                    // report one does anything, and it says so.
                    component Tag: Rectangle {
                        property string label: ""
                        property color tint: App.colour.textMuted
                        property bool clickable: false
                        /// Exposed, because the MouseArea's id is not in scope
                        /// where the component is used -- a tooltip written
                        /// there cannot see it.
                        readonly property bool hovered: mouse.containsMouse
                        signal activated()

                        radius: 3
                        implicitWidth: tagText.implicitWidth + 14
                        implicitHeight: 20
                        color: mouse.containsMouse && clickable ? App.colour.hover : "transparent"
                        border.width: 1
                        border.color: tint

                        Label {
                            id: tagText
                            anchors.centerIn: parent
                            text: parent.label
                            color: parent.tint
                            font.pixelSize: 10
                        }

                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: parent.clickable
                            cursorShape: Qt.PointingHandCursor
                            onClicked: parent.activated()
                        }
                    }

                    Tag {
                        objectName: "reportTag"
                        visible: controller ? controller.hasReport : false
                        clickable: true
                        tint: App.colour.link
                        label: "report · " + (controller ? controller.reportAgeText : "")
                        // The saved report, not a fresh scan: the point of a
                        // history is not to walk the tree again to look at it.
                        onActivated: App.openReportFor(controller.activePane.currentUri)
                        ToolTip.visible: hovered
                        ToolTip.text: "Open the saved report for this folder"
                    }

                    Tag {
                        objectName: "alertTag"
                        visible: controller ? controller.alertCount > 0 : false
                        tint: controller && controller.triggeredAlertCount > 0 ? App.colour.bad : App.colour.textMuted
                        label: {
                            if (!controller)
                                return ""
                            // "3 alert triggered" was a concatenation with no
                            // plural branch, directly above a line that has one.
                            // See App.countOf() and MOLE-398.
                            if (controller.triggeredAlertCount > 0)
                                return App.countOf(controller.triggeredAlertCount,
                                                   "alert", "alerts") + " triggered"
                            return App.countOf(controller.alertCount, "alert", "alerts")
                        }
                    }

                    Tag {
                        objectName: "accessTag"
                        visible: controller ? controller.accessKnown : false
                        tint: controller && controller.readOnlyHere ? App.colour.warn : App.colour.textMuted
                        label: controller ? controller.accessText : ""
                        ToolTip.visible: hovered
                        ToolTip.text: controller && controller.accessDetail.length > 0
                                      ? controller.accessDetail : "Permissions here"
                    }

                    Tag {
                        objectName: "indexTag"
                        visible: controller ? controller.indexed : false
                        tint: App.colour.ok
                        label: controller ? controller.indexedText : ""
                    }

                    Tag {
                        objectName: "notIndexedTag"
                        visible: controller ? !controller.indexed : false
                        tint: App.colour.textFaint
                        label: "not indexed"
                        ToolTip.visible: hovered
                        ToolTip.text: "Tools ▸ Index this folder makes it searchable instantly"
                    }
                }

                ToolSeparator {}

                Label {
                    text: "F2 rename · F7 folder · F8 delete · Ins select · Tab switch"
                    color: App.colour.textFaint
                    font.pixelSize: 11
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            // The same divider as the one beside the sidebar, and here for the
            // same reason: the style's own is painted from `Material.background`,
            // which the window no longer hands down. See Main.qml and ADR-0074.
            handle: Rectangle {
                implicitWidth: 6
                implicitHeight: 6
                color: SplitHandle.pressed
                       ? App.colour.window
                       : Qt.lighter(App.colour.window, SplitHandle.hovered ? 1.2 : 1.1)

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

            FilePane {
                id: leftPane
                SplitView.fillWidth: true
                SplitView.minimumWidth: 260
                tileMode: controller ? controller.tilesEnabled : false
                tileWidth: controller ? controller.tileWidth : 132
                tileHeight: controller ? controller.tileHeight : 104
                paneController: controller ? controller.left : null
                active: controller ? controller.activePaneIndex === 0 : true
                onFocusRequested: if (controller) controller.activePaneIndex = 0
                onTransferRequested: function(move) { view.requestTransfer(move) }
                onDropNeedsAnswer: function(urls, plan) {
                    transferDialog.prepareDrop(leftPane.paneController, urls, plan)
                }
                onSwitchPaneRequested: {
                    if (controller && controller.splitEnabled) {
                        controller.focusOtherPane()
                        rightPane.takeFocus()
                    }
                }
            }

            FilePane {
                id: rightPane
                visible: controller ? controller.splitEnabled : false
                tileMode: false
                SplitView.preferredWidth: parent.width / 2
                SplitView.minimumWidth: 260
                paneController: controller ? controller.right : null
                active: controller ? controller.activePaneIndex === 1 : false
                onFocusRequested: if (controller) controller.activePaneIndex = 1
                onTransferRequested: function(move) { view.requestTransfer(move) }
                onDropNeedsAnswer: function(urls, plan) {
                    transferDialog.prepareDrop(rightPane.paneController, urls, plan)
                }
                onSwitchPaneRequested: {
                    if (controller) {
                        controller.focusOtherPane()
                        leftPane.takeFocus()
                    }
                }
            }
        }
    }

    // --- dialogs -------------------------------------------------------

    MoleDialog {
        id: transferDialog
        objectName: "transferDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        property bool isMove: false
        /// Read once when the dialog opens; the panes cannot change underneath
        /// a modal prompt, and re-reading per binding would re-scan the listing.
        property var plan: ({})
        /// Set when the question came from a drop rather than from F5. The same
        /// dialog either way -- a count, a size, a destination, the names that
        /// clash and what to do about them are the same question however it was
        /// asked, and a second dialog that looked like this one would be one more
        /// place for the wording to drift.
        property bool isDrop: false
        property var dropTargetPane: null
        property var droppedUrls: []

        title: isMove ? "Move" : "Copy"
        preferredWidth: 520

        footer: ConfirmButtons {
            acceptText: transferDialog.isMove ? "Move" : "Copy"
            // Overwriting is the one answer here that cannot be undone, and the
            // dialog already says so in red under the box. Choosing it turns the
            // acting button red and takes the keyboard off it, the way the delete
            // confirmation behaves.
            destructive: conflictBox.currentValue === "overwrite"
            // A single item can be renamed on arrival, and a dialog with a field
            // in it is typed into first.
            keyboardOn: nameRow.visible ? "none" : (destructive ? "reject" : "accept")
        }

        function prepare(move) {
            isMove = move
            isDrop = false
            dropTargetPane = null
            droppedUrls = []
            plan = controller ? controller.transferPlan() : ({})
            nameField.text = plan.singleName ? plan.singleName : ""
            // Default to stopping. A prompt whose safe answer is not the default
            // is a prompt that will one day overwrite something by reflex.
            conflictBox.currentIndex = 0
            open()
        }

        /// The same question, asked by a drop. `paneDropped` is the pane the files
        /// landed on and `dropPlan` is what it said would happen -- read at the
        /// moment of the drop, so what is agreed to and what happens are the same
        /// files.
        function prepareDrop(paneDropped, urls, dropPlan) {
            isMove = false
            isDrop = true
            dropTargetPane = paneDropped
            droppedUrls = urls
            plan = dropPlan
            conflictBox.currentIndex = 0
            open()
        }

        // After opening, not in prepare(): the field does not exist to take the
        // keyboard until the dialog is up.
        onOpened: if (nameRow.visible) nameField.forceActiveFocus()

        onAccepted: {
            if (isDrop) {
                if (dropTargetPane)
                    dropTargetPane.dropHere(droppedUrls, conflictBox.currentValue)
            } else {
                controller.runTransfer(isMove, nameRow.visible ? nameField.text : "",
                                       conflictBox.currentValue)
            }
            view.focusActivePane()
        }
        onRejected: view.focusActivePane()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: {
                    const count = transferDialog.plan.count || 0
                    const size = transferDialog.plan.sizeText || ""
                    const where = transferDialog.plan.targetPath || ""
                    const what = App.countOf(count, "item", "items")
                    return what + (size.length > 0 ? " · " + size : "") + "\n→ " + where
                }
            }

            // A single item can arrive under a different name. A batch cannot,
            // because there would be nothing sensible to call the rest.
            //
            // The condition is on the row and nowhere else. It used to be on the
            // field, with the row bound to `nameField.visible` -- and `visible`
            // on an item is its effective visibility, parents included, so the
            // row was waiting on the field and the field was waiting on the row.
            // Both settled on false and the field never appeared at all: renaming
            // a single file on the way across has never once worked.
            RowLayout {
                id: nameRow
                Layout.fillWidth: true
                // Not for a drop: what arrives keeps the name it had. Renaming on
                // the way in is a rename after the copy, and it is one keystroke
                // away once the file is here.
                visible: !transferDialog.isDrop
                         && (transferDialog.plan.singleName || "").length > 0
                Label {
                    text: "Name"
                    color: App.colour.textMuted
                    font.pixelSize: 12
                }
                TextField {
                    id: nameField
                    objectName: "transferName"
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    selectByMouse: true
                    // The keyboard is in here, so Return has to answer the
                    // dialog from here -- the same as the rename and mkdir
                    // fields. A field that swallows Return is a dialog that
                    // cannot be answered without the mouse.
                    onAccepted: transferDialog.accept()
                }
            }

            // Named, before anything happens. Discovering an overwrite
            // afterwards is discovering it too late.
            Rectangle {
                objectName: "collisionWarning"
                Layout.fillWidth: true
                visible: (transferDialog.plan.collisions || []).length > 0
                radius: 4
                color: Qt.alpha(App.colour.warn, 0.16)
                border.color: App.colour.warn
                implicitHeight: collisionText.implicitHeight + 16

                Label {
                    id: collisionText
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: Text.Wrap
                    color: App.colour.warn
                    font.pixelSize: 11
                    text: {
                        const names = transferDialog.plan.collisions || []
                        if (names.length === 0)
                            return ""
                        const head = names.length === 1
                                     ? "1 name already exists there:"
                                     : names.length + " names already exist there:"
                        const shown = names.slice(0, 8).join(", ")
                        return head + "  " + shown
                               + (names.length > 8 ? ", and " + (names.length - 8) + " more" : "")
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "If it already exists"
                    color: App.colour.textMuted
                    font.pixelSize: 12
                }
                Picker {
                    id: conflictBox
                    objectName: "conflictStrategy"
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    textRole: "label"
                    valueRole: "id"
                    model: [
                        { id: "stop", label: "Stop and report it" },
                        { id: "skip", label: "Skip that file, transfer the rest" },
                        { id: "overwrite", label: "Overwrite it" }
                    ]
                }
            }

            Label {
                Layout.fillWidth: true
                visible: conflictBox.currentValue === "overwrite"
                wrapMode: Text.Wrap
                color: App.colour.bad
                font.pixelSize: 11
                text: "Overwriting replaces the file at the destination. There is no undo."
            }
        }
    }

    MoleDialog {
        id: transferHint
        objectName: "transferHint"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        title: "Two panes needed"
        preferredWidth: 420

        footer: ConfirmButtons { dismissOnly: true }

        // Either way out, since there is only the one and Escape is the other
        // half of it.
        onClosed: view.focusActivePane()

        Label {
            anchors.fill: parent
            wrapMode: Text.Wrap
            text: "Copying and moving need somewhere to copy to. Switch to Dual and point the other pane at the destination."
        }
    }

    // The window's toast, in this view. It was a Popup of its own with its own
    // timer and its own five numbers -- and without the one line that keeps a
    // popup from taking the keyboard, so every window shortcut stopped working
    // for the six seconds an error was on screen. See ui/Toast.qml and MOLE-398.
    Toast {
        id: errorPopup
        objectName: "errorPopup"
        dwellMs: 6000
        textColour: App.colour.bad
    }
}
