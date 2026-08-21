import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What runs on its own, and whether it worked.
//
// Failures lead, because automation nobody checks is automation nobody can
// trust: a report that quietly stopped refreshing three weeks ago is worse
// than never having scheduled it.
Item {
    id: view

    property var controller: null

    function colorForStatus(status) {
        if (status === "failed" || status === "skipped")
            return App.colour.bad
        if (status === "succeeded")
            return App.colour.ok
        if (status === "running")
            return Material.accent
        return App.colour.textMuted
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "Scheduled jobs"
                font.pixelSize: 20
                font.bold: true
            }

            Rectangle {
                objectName: "automationFailureBadge"
                visible: view.controller && view.controller.failingCount > 0
                radius: 4
                color: Qt.alpha(App.colour.bad, 0.16)
                border.color: App.colour.bad
                implicitWidth: failureLabel.implicitWidth + 16
                implicitHeight: failureLabel.implicitHeight + 8
                Label {
                    id: failureLabel
                    anchors.centerIn: parent
                    color: App.colour.bad
                    text: {
                        if (!view.controller)
                            return ""
                        const n = view.controller.failingCount
                        return n === 1 ? "1 job needs attention"
                                       : n + " jobs need attention"
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                color: App.colour.textMuted
                text: view.controller && view.controller.runningCount > 0
                      ? view.controller.runningCount + " running now" : ""
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !view.controller || view.controller.rules.length === 0
            color: App.colour.textMuted
            wrapMode: Text.WordWrap
            text: "Nothing is scheduled yet.\n\n" +
                  "Open a folder report and choose how often it should be repeated. " +
                  "A job whose turn came while the application was closed runs the next time it starts, " +
                  "so nothing is skipped just because the machine was off."
        }

        // ---- the rules -----------------------------------------------------

        ListView {
            objectName: "scheduleList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, view.height * 0.5)
            visible: view.controller && view.controller.rules.length > 0
            clip: true
            spacing: 6
            model: view.controller ? view.controller.rules : []

            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                implicitHeight: ruleBody.implicitHeight + 20
                radius: 6
                color: App.colour.panel
                border.width: 1
                border.color: modelData.failing ? App.colour.bad : App.colour.border

                ColumnLayout {
                    id: ruleBody
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: view.colorForStatus(modelData.status)
                        }

                        Label {
                            text: modelData.label
                            font.bold: true
                            elide: Text.ElideMiddle
                            Layout.maximumWidth: 320
                        }

                        Label {
                            color: view.colorForStatus(modelData.status)
                            text: modelData.statusText
                                  + (modelData.consecutiveFailures > 1
                                     ? " (" + modelData.consecutiveFailures + "×)" : "")
                        }

                        Item { Layout.fillWidth: true }

                        Picker {
                            objectName: "intervalPicker"
                            model: view.controller ? view.controller.intervalPresets : []
                            textRole: "label"
                            valueRole: "seconds"
                            currentIndex: {
                                const presets = view.controller ? view.controller.intervalPresets : []
                                for (let i = 0; i < presets.length; ++i) {
                                    if (presets[i].seconds === modelData.intervalSeconds)
                                        return i
                                }
                                return -1
                            }
                            displayText: currentIndex >= 0 ? currentText : modelData.intervalText
                            onActivated: view.controller.setInterval(modelData.id, currentValue)
                        }

                        Button {
                            text: modelData.running ? "Running…" : "Run now"
                            enabled: !modelData.running
                            onClicked: view.controller.runNow(modelData.id)
                        }

                        Switch {
                            objectName: "ruleEnabled"
                            checked: modelData.enabled
                            onToggled: view.controller.setEnabled(modelData.id, checked)
                            ToolTip.text: "Pause without deleting the rule"
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                        }

                        ToolButton {
                            text: "×"
                            font.pixelSize: App.textSize
                            onClicked: view.controller.removeRule(modelData.id)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        color: App.colour.textMuted
                        elide: Text.ElideMiddle
                        text: modelData.target
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 18
                        Label {
                            color: App.colour.textMuted
                            text: "Last run: " + modelData.lastRunText
                        }
                        Label {
                            color: App.colour.textMuted
                            text: "Next: " + modelData.nextDueText
                        }
                        Item { Layout.fillWidth: true }
                    }

                    // Only shown when there is something to explain -- a green
                    // job does not need a paragraph about being green.
                    Label {
                        Layout.fillWidth: true
                        visible: modelData.failing && modelData.message.length > 0
                        color: App.colour.bad
                        wrapMode: Text.WordWrap
                        text: modelData.message
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: App.colour.border
            visible: view.controller && view.controller.rules.length > 0
        }

        // ---- the run log ---------------------------------------------------

        RowLayout {
            Layout.fillWidth: true
            visible: view.controller && view.controller.history.length > 0
            spacing: 12

            Label {
                text: "Run history"
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Clear"
                flat: true
                onClicked: view.controller.clearHistory()
            }
        }

        ListView {
            objectName: "runHistoryList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.controller && view.controller.history.length > 0
            clip: true
            model: view.controller ? view.controller.history : []

            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 12

                Rectangle {
                    width: 6
                    height: 6
                    radius: 3
                    color: view.colorForStatus(modelData.status)
                }
                Label {
                    text: modelData.startedAt
                    color: App.colour.textMuted
                    font.family: App.monospaceFont
                }
                Label {
                    text: modelData.label
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 220
                }
                Label {
                    text: modelData.statusText
                    color: view.colorForStatus(modelData.status)
                }
                Label {
                    text: modelData.durationText
                    color: App.colour.textMuted
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.message
                    color: modelData.failed ? App.colour.bad : App.colour.textMuted
                    elide: Text.ElideRight
                }
            }
        }
    }
}
