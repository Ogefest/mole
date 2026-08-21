import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What a file says about itself, down the page.
//
// One component, used twice: in the drawer beside every viewer, and in the body
// of the information viewer, whose content the facts *are* -- a file nothing can
// show has nothing else to say, so it must not go blank when the drawer is put
// away. See docs/adr/0034-what-a-file-says-about-itself.md.
//
// The values are selectable, which is the whole point of a fact nobody can copy:
// a camera model, a serial number or a full path is something people take out of
// here and paste somewhere else.
Item {
    id: list

    /// One map per fact: `label`, `value`, and `startsBlock` on the first row of
    /// each reader's contribution.
    property var facts: []
    property bool busy: false
    /// Called by the copy-all button. Set by whoever owns the facts.
    property var onCopyAll: null

    readonly property int labelWidth: 108

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 6
            Layout.topMargin: 6
            spacing: 6

            Label {
                text: "Details"
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }
            BusyIndicator {
                running: list.busy
                visible: running
                implicitWidth: 14
                implicitHeight: 14
            }
            Item { Layout.fillWidth: true }

            ToolButton {
                objectName: "copyAllDetails"
                text: "Copy all"
                visible: list.facts.length > 0 && list.onCopyAll !== null
                font.pixelSize: App.smallTextSize
                focusPolicy: Qt.NoFocus
                ToolTip.text: "Every row as label: value lines"
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: list.onCopyAll()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                objectName: "detailsRows"
                model: list.facts
                boundsBehavior: Flickable.StopAtBounds

                delegate: Column {
                    required property var modelData
                    width: ListView.view.width

                    // A hairline where one reader's answer ends and the next
                    // begins. Nothing is regrouped or reordered: the order is
                    // the readers' priority order.
                    Rectangle {
                        visible: modelData.startsBlock === true
                        width: parent.width - 20
                        x: 10
                        height: 1
                        color: App.colour.border
                    }

                    Row {
                        width: parent.width
                        leftPadding: 10
                        rightPadding: 10
                        topPadding: 3
                        bottomPadding: 3
                        spacing: 8

                        Label {
                            width: list.labelWidth
                            text: modelData.label
                            color: App.colour.textFaint
                            font.pixelSize: App.smallTextSize
                            wrapMode: Text.Wrap
                        }

                        // A read-only editor rather than a Label, because a
                        // Label cannot be selected and a value nobody can copy
                        // is a value somebody retypes. Wraps rather than elides:
                        // a full path is always longer than the drawer.
                        TextEdit {
                            width: parent.width - list.labelWidth - 28
                            text: modelData.value
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: App.colour.textSecondary
                            selectionColor: Material.accent
                            font.pixelSize: App.smallTextSize
                        }
                    }
                }
            }
        }
    }
}
