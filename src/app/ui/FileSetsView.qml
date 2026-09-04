import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A set: a list of files somebody built by hand, treated as one thing.
//
// Sets on the left, members on the right. Everything the application can do to a
// selection it can do to a set, because a set answers the same question a pane's
// selection does — which is why there is nothing here about copying or reporting.
Item {
    id: view
    objectName: "setsView"
    property var controller: null

    function focusActivePane() { body.forceActiveFocus() }

    // ---- the cursor over the members ---------------------------------------
    //
    // A list of its own rather than the browser's FileListModel. That model backs
    // the browser panes and the search results, and teaching it what a set is
    // would put a third tenant into shared code on behalf of a view that wants a
    // cursor and nothing else. The cost is real and is this: no sorting by
    // column, no selecting several members, no type-to-filter. None of the three
    // existed here before either. See MOLE-205.
    //
    // No `activePane` on the controller, deliberately. It would have made these
    // keys work with no view code at all, and it would also have switched on four
    // menu actions whose only condition is that the property exists -- one of
    // which reads a location a set does not have. See ADR-0060.

    readonly property var memberRows: controller ? controller.members : []

    /// Where the cursor is, as a uri rather than a row number.
    ///
    /// The member list is rebuilt from scratch whenever anything about the set
    /// changes -- a check finishing, a member removed, the filter narrowed -- and
    /// a ListView handed a new model puts its cursor back on the first row. A row
    /// number would quietly come to mean a different file; a uri either is still
    /// in the list or is not.
    property string cursorUri: ""

    /// The row the cursor is on now: the remembered member while it is still
    /// there, otherwise the first one, because arriving at a list of files should
    /// land on one rather than on nothing.
    readonly property int cursorRow: {
        if (view.memberRows.length === 0)
            return -1
        for (var i = 0; i < view.memberRows.length; ++i) {
            if (view.memberRows[i].uri === view.cursorUri)
                return i
        }
        return 0
    }

    readonly property var currentMember:
        view.cursorRow >= 0 ? view.memberRows[view.cursorRow] : null

    // The three functions the window's key fallback looks for by name. See
    // Main.qml and ADR-0060.

    function moveCursorBy(delta) {
        if (view.memberRows.length === 0)
            return
        var next = Math.max(0, Math.min(view.memberRows.length - 1, view.cursorRow + delta))
        view.cursorUri = view.memberRows[next].uri
        memberList.positionViewAtIndex(next, ListView.Contain)
    }

    function activateCurrentRow() {
        var member = view.currentMember
        if (!member)
            return
        // A member whose file has gone has nothing to open, and opening the folder
        // it used to be in -- which is what the double click did -- reads as
        // though the file were still there.
        if (member.missing) {
            if (controller)
                controller.reportMissing(member.uri)
            return
        }
        // The member itself, not its folder: a browser opens with the cursor on
        // it, so the next key acts on the file that was asked for.
        App.revealFile(member.uri)
    }

    function previewCurrentRow() {
        var member = view.currentMember
        if (!member)
            return
        if (member.missing) {
            if (controller)
                controller.reportMissing(member.uri)
            return
        }
        App.previewFile(member.uri)
    }

    FocusScope {
        id: body
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: controller && controller.currentName.length > 0
                          ? controller.currentName : "Sets"
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    objectName: "setSummary"
                    text: controller ? controller.summary : ""
                    color: App.colour.textMuted
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "Check"
                    flat: true
                    enabled: controller && controller.memberCount > 0
                    onClicked: controller.verify()
                    ToolTip.visible: hovered
                    ToolTip.text: "Look for members whose file has gone"
                }

                Button {
                    text: "Forget missing"
                    flat: true
                    visible: controller && controller.missingCount > 0
                    onClicked: controller.forgetMissing()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: !controller || controller.sets.length === 0
                color: App.colour.textMuted
                wrapMode: Text.WordWrap
                text: "No sets yet.\n\n" +
                      "A set is a list of files you build by hand — from anywhere, across any " +
                      "number of drives — and then work on as one thing: analyse it, search it, " +
                      "copy it, rename its contents.\n\n" +
                      "Name one below, then add files to it from a browser tab."
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: nameField
                    objectName: "newSetName"
                    Layout.preferredWidth: 220
                    font.pixelSize: 12
                    placeholderText: "New set name"
                    onAccepted: createButton.clicked()
                }
                Button {
                    id: createButton
                    objectName: "createSetButton"
                    text: "Create"
                    enabled: nameField.text.trim().length > 0
                    onClicked: {
                        if (controller.createSet(nameField.text).length > 0)
                            nameField.text = ""
                    }
                }

                Item { Layout.fillWidth: true }

                TextField {
                    objectName: "setMemberFilter"
                    Layout.preferredWidth: 220
                    visible: controller && controller.memberCount > 0
                    font.pixelSize: 12
                    placeholderText: "Filter members…"
                    text: controller ? controller.filter : ""
                    onTextEdited: if (controller) controller.filter = text
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: controller && controller.sets.length > 0
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 240
                    Layout.fillHeight: true
                    radius: 6
                    color: App.colour.panel
                    border.width: 1
                    border.color: App.colour.border

                    ListView {
                        objectName: "setList"
                        anchors.fill: parent
                        anchors.margins: 6
                        clip: true
                        spacing: 2
                        model: controller ? controller.sets : []

                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: 34
                            radius: 4
                            color: modelData.current ? App.colour.selection
                                 : setMouse.containsMouse ? App.colour.hover : "transparent"

                            MouseArea {
                                id: setMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: controller.currentSetId = modelData.id
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 4
                                spacing: 6

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Text.ElideMiddle
                                        font.pixelSize: 13
                                        font.bold: modelData.current
                                    }
                                    Label {
                                        text: App.countOf(modelData.count, "item", "items")
                                              + (modelData.driveCount > 1
                                                 ? " · " + App.countOf(modelData.driveCount,
                                                                       "drive", "drives") : "")
                                        color: App.colour.textMuted
                                        font.pixelSize: 10
                                    }
                                }

                                ToolButton {
                                    text: "×"
                                    font.pixelSize: App.textSize
                                    visible: setMouse.containsMouse
                                    implicitWidth: App.minimumTarget
                                    implicitHeight: App.minimumTarget
                                    onClicked: controller.removeSet(modelData.id)
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        visible: controller && controller.memberCount === 0
                        color: App.colour.textMuted
                        wrapMode: Text.WordWrap
                        text: "This set is empty. Select files in a browser tab and use " +
                              "Tools ▸ Add to set."
                    }

                    ListView {
                        id: memberList
                        objectName: "setMemberList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: view.memberRows
                        // No currentIndex of its own, deliberately. A ListView
                        // handed a new model sets that property itself, which
                        // replaces a binding's value without removing the binding
                        // -- so the highlight would stay on the first row until
                        // something else happened to move it. view.cursorRow is
                        // the one cursor there is.

                        delegate: Rectangle {
                            id: memberRow
                            objectName: "setMemberRow"
                            required property var modelData
                            required property int index
                            width: ListView.view.width
                            implicitHeight: 30
                            color: memberRow.index === view.cursorRow ? App.colour.selection
                                 : memberMouse.containsMouse ? App.colour.hover : "transparent"

                            MouseArea {
                                id: memberMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                // One click moves the cursor, so the mouse and the
                                // keyboard agree about where the work is.
                                onClicked: view.cursorUri = memberRow.modelData.uri
                                // And a double click does what Enter does: opens the
                                // member. It used to open the member's *folder*,
                                // which left the person finding the file again by
                                // hand.
                                onDoubleClicked: {
                                    view.cursorUri = memberRow.modelData.uri
                                    view.activateCurrentRow()
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 10

                                Label {
                                    text: modelData.missing ? "✕" : (modelData.checked ? "✓" : "·")
                                    color: modelData.missing ? App.colour.bad
                                         : modelData.checked ? App.colour.ok : App.colour.textFaint
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    color: modelData.missing ? App.colour.bad : App.colour.textSecondary
                                    Layout.preferredWidth: 220
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.location
                                    color: App.colour.textMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.sizeText
                                    color: App.colour.textMuted
                                    font.pixelSize: 11
                                }
                                ToolButton {
                                    text: "×"
                                    font.pixelSize: App.textSize
                                    visible: memberMouse.containsMouse
                                    implicitWidth: App.minimumTarget
                                    implicitHeight: App.minimumTarget
                                    onClicked: controller.removeUris([modelData.uri])
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
