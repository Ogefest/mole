import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: strip

    property color panelColor: "#1b2029"
    property color mutedText: "#8b93a7"
    property bool expanded: false

    readonly property bool working: App.tasks.activeCount > 0

    // Tinted while something is running. A count alone read as decoration; the
    // point of this strip is that work happening in the background should be
    // impossible to mistake for nothing happening.
    color: working ? "#1e2a3a" : panelColor

    // An accent rule along the top, for the same reason.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 2
        color: Material.accent
        visible: strip.working
    }

    // Measured, not guessed. The collapsed strip has to fit a row of buttons,
    // and a hard-coded 34px clipped them on every style whose controls are
    // taller than that.
    implicitHeight: expanded ? header.implicitHeight + 160 + 18
                             : header.implicitHeight + 12

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        RowLayout {
            id: header
            Layout.fillWidth: true
            spacing: 8

            ToolButton {
                text: strip.expanded ? "▾" : "▸"
                implicitWidth: 24
                implicitHeight: 22
                onClicked: strip.expanded = !strip.expanded
            }

            BusyIndicator {
                visible: strip.working
                running: visible
                implicitWidth: 18
                implicitHeight: 18
            }

            Label {
                text: {
                    if (App.tasks.count === 0)
                        return "No background work"
                    if (!strip.working)
                        return App.tasks.count + " finished"
                    return App.tasks.activeCount === 1
                           ? "1 running" : App.tasks.activeCount + " running"
                }
                color: strip.working ? "#e6ebf5" : strip.mutedText
                font.pixelSize: 12
                font.bold: strip.working
            }

            // What is happening, not just how many things are. Collapsed, this
            // is the only thing on screen that says so.
            Label {
                objectName: "activeTaskTitle"
                visible: strip.working && !strip.expanded
                Layout.maximumWidth: 320
                text: App.tasks.activeTitle
                color: "#c9d1e0"
                elide: Text.ElideMiddle
                font.pixelSize: 12
            }

            ProgressBar {
                objectName: "activeTaskProgress"
                visible: strip.working && !strip.expanded
                Layout.preferredWidth: 140
                indeterminate: App.tasks.activeProgress < 0
                from: 0
                to: 100
                value: Math.max(0, App.tasks.activeProgress)
            }

            Label {
                visible: strip.working && !strip.expanded
                Layout.maximumWidth: 200
                text: App.tasks.activeStatus
                color: strip.mutedText
                elide: Text.ElideRight
                font.pixelSize: 11
            }

            // How fast and how long. A copy of a large file is otherwise a bar
            // with no way to tell whether it is moving or merely present.
            Label {
                objectName: "activeTaskRate"
                visible: strip.working && App.tasks.activeRateText.length > 0
                text: App.tasks.activeRateText
                color: "#7cc4ff"
                font.pixelSize: 11
                font.family: App.monospaceFont
            }

            Label {
                objectName: "activeTaskElapsed"
                visible: strip.working && App.tasks.activeElapsedText.length > 0
                text: App.tasks.activeElapsedText
                color: strip.mutedText
                font.pixelSize: 11
                font.family: App.monospaceFont
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                visible: App.tasks.activeCount > 0
                text: "Cancel all"
                font.pixelSize: 12
                onClicked: App.tasks.cancelAll()
            }

            ToolButton {
                visible: App.tasks.finishedCount > 0
                text: "Clear finished"
                font.pixelSize: 12
                onClicked: App.tasks.clearFinished()
                ToolTip.visible: hovered
                ToolTip.text: "Finished work leaves this list by itself after an hour"
                ToolTip.delay: 600
            }
        }

        ListView {
            visible: strip.expanded
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: App.tasks
            spacing: 2

            delegate: RowLayout {
                required property int index
                required property string title
                required property string stateText
                required property string statusText
                required property int progress
                required property bool canCancel
                required property bool isFinished
                required property string startedAtText
                required property string elapsedText
                required property var metrics

                width: ListView.view ? ListView.view.width : 0
                spacing: 8

                Label {
                    Layout.preferredWidth: 220
                    text: title
                    elide: Text.ElideMiddle
                    font.pixelSize: 12
                }

                Label {
                    Layout.preferredWidth: 62
                    text: startedAtText
                    color: strip.mutedText
                    font.pixelSize: 11
                    font.family: App.monospaceFont
                }

                ProgressBar {
                    Layout.preferredWidth: 120
                    // A finished task has no progress to animate. A negative
                    // value means "unknown", which is right while running and
                    // wrong afterwards -- a cancelled scan was left with a bar
                    // sweeping for ever, as though it were still going.
                    visible: !isFinished || progress >= 0
                    indeterminate: !isFinished && progress < 0
                    from: 0
                    to: 100
                    value: isFinished && progress < 0 ? 0 : Math.max(0, progress)
                }

                // Something has to occupy the column when there is no bar, or
                // the row's remaining fields jump left.
                Item {
                    Layout.preferredWidth: 120
                    visible: isFinished && progress < 0
                }

                Label {
                    Layout.preferredWidth: 56
                    text: elapsedText
                    color: strip.mutedText
                    font.pixelSize: 11
                    font.family: App.monospaceFont
                }

                Label {
                    Layout.preferredWidth: 200
                    text: statusText.length > 0 ? statusText : stateText
                    color: strip.mutedText
                    elide: Text.ElideRight
                    font.pixelSize: 12
                }

                // Whatever the task chose to publish. The strip lays it out
                // without knowing what any of it means, which is what lets a
                // new kind of task report a new kind of number.
                Row {
                    Layout.fillWidth: true
                    spacing: 10

                    Repeater {
                        model: metrics
                        delegate: Label {
                            required property var modelData
                            text: modelData.label + " " + modelData.text
                            color: strip.mutedText
                            font.pixelSize: 11
                        }
                    }
                }

                ToolButton {
                    visible: canCancel
                    text: "×"
                    implicitWidth: 22
                    implicitHeight: 22
                    onClicked: App.tasks.cancel(index)
                }
            }
        }
    }
}
