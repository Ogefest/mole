import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What is being watched, what tripped, and the form for watching one more.
//
// All three in one place on purpose: an alert defined somewhere you cannot see
// the others is an alert you forget you set.
Item {
    id: view
    property var controller: null

    function colorForState(state) {
        if (state === "triggered")
            return App.colour.bad
        if (state === "failed")
            return App.colour.warn
        if (state === "ok")
            return App.colour.ok
        return App.colour.textMuted
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "Alerts"
                font.pixelSize: 20
                font.bold: true
            }

            Rectangle {
                objectName: "alertBadge"
                visible: controller && controller.triggeredCount > 0
                radius: 4
                color: Qt.alpha(App.colour.bad, 0.16)
                border.color: App.colour.bad
                implicitWidth: badgeLabel.implicitWidth + 16
                implicitHeight: badgeLabel.implicitHeight + 8
                Label {
                    id: badgeLabel
                    anchors.centerIn: parent
                    color: App.colour.bad
                    text: controller
                          ? (controller.triggeredCount === 1 ? "1 triggered"
                                                             : controller.triggeredCount + " triggered")
                          : ""
                }
            }

            Item { Layout.fillWidth: true }

            BusyIndicator {
                running: controller ? controller.busy : false
                visible: running
                implicitWidth: 18
                implicitHeight: 18
            }

            Button {
                text: "Check all now"
                flat: true
                onClicked: controller.checkNow("")
            }
        }

        // ---- watch something new ------------------------------------------

        Rectangle {
            Layout.fillWidth: true
            radius: 6
            color: App.colour.panel
            border.width: 1
            border.color: App.colour.border
            implicitHeight: form.implicitHeight + 24

            GridLayout {
                id: form
                anchors.fill: parent
                anchors.margins: 12
                columns: 4
                columnSpacing: 10
                rowSpacing: 8

                Label {
                    Layout.columnSpan: 4
                    text: "Watch something"
                    color: App.colour.textMuted
                    font.pixelSize: 11
                    font.letterSpacing: 1
                }

                TextField {
                    objectName: "alertTarget"
                    id: targetField
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    placeholderText: "File, folder or drive"
                    text: controller ? controller.suggestedTarget : ""
                    font.pixelSize: 12
                }

                Picker {
                    objectName: "alertSource"
                    id: sourceBox
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    textRole: "label"
                    valueRole: "id"
                    model: controller ? controller.sourceChoices : []
                }

                TextField {
                    objectName: "alertLabel"
                    id: labelField
                    Layout.fillWidth: true
                    placeholderText: "Name (optional)"
                    font.pixelSize: 12
                }

                Picker {
                    objectName: "alertMetric"
                    id: metricBox
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    textRole: "label"
                    valueRole: "id"
                    model: controller ? controller.metricChoices : []
                }

                Picker {
                    objectName: "alertComparison"
                    id: comparisonBox
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    textRole: "label"
                    valueRole: "id"
                    model: controller ? controller.comparisonChoices : []
                }

                // Hidden for "changes", which has nothing to compare against.
                TextField {
                    objectName: "alertThreshold"
                    id: thresholdField
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    enabled: comparisonBox.currentValue !== "changed"
                             && (controller ? controller.metricNeedsNumber(metricBox.currentValue) : true)
                    placeholderText: {
                        if (!controller)
                            return ""
                        const unit = controller.unitFor(metricBox.currentValue)
                        if (unit === "bytes")
                            return "e.g. 10 GB"
                        return unit.length > 0 ? "e.g. 100 " + unit : "value"
                    }
                }

                Button {
                    objectName: "addAlertButton"
                    text: "Add"
                    enabled: targetField.text.trim().length > 0
                    onClicked: {
                        const threshold = controller.parseThreshold(thresholdField.text,
                                                                    metricBox.currentValue)
                        const id = controller.addAlert(labelField.text, targetField.text,
                                                       metricBox.currentValue,
                                                       comparisonBox.currentValue,
                                                       threshold, sourceBox.currentValue)
                        if (id.length > 0) {
                            labelField.text = ""
                            thresholdField.text = ""
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !controller || controller.alerts.length === 0
            color: App.colour.textMuted
            wrapMode: Text.WordWrap
            text: "Nothing is being watched yet.\n\n" +
                  "Point an alert at a folder and pick what matters: how big it is getting, " +
                  "how much room is left on the drive, whether its permissions changed, or how " +
                  "long it has been since anything in it was touched — a backup that quietly " +
                  "stopped running looks exactly like one that is working, until you check.\n\n" +
                  "An alert reading from the latest report is instant but only as fresh as that " +
                  "report, which is what scheduling the report is for."
        }

        // ---- what is watched -----------------------------------------------

        ListView {
            objectName: "alertList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, view.height * 0.4)
            visible: controller && controller.alerts.length > 0
            clip: true
            spacing: 6
            model: controller ? controller.alerts : []

            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                implicitHeight: alertBody.implicitHeight + 20
                radius: 6
                color: App.colour.panel
                border.width: 1
                border.color: modelData.triggered ? App.colour.bad
                            : modelData.failed ? App.colour.warn : App.colour.border

                ColumnLayout {
                    id: alertBody
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: view.colorForState(modelData.state)
                        }
                        Label {
                            text: modelData.label
                            font.bold: true
                            elide: Text.ElideMiddle
                            Layout.maximumWidth: 340
                        }
                        Label {
                            color: view.colorForState(modelData.state)
                            text: modelData.stateText
                                  + (modelData.value.length > 0 ? "  ·  " + modelData.value : "")
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            text: "Check"
                            flat: true
                            onClicked: controller.checkNow(modelData.id)
                        }
                        Switch {
                            checked: modelData.enabled
                            onToggled: controller.setEnabled(modelData.id, checked)
                        }
                        ToolButton {
                            text: "×"
                            font.pixelSize: App.textSize
                            onClicked: controller.removeAlert(modelData.id)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        color: App.colour.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        text: modelData.condition + "  ·  " + modelData.sourceText
                              + "  ·  checked " + modelData.lastCheckedText
                    }

                    Label {
                        Layout.fillWidth: true
                        color: App.colour.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        text: modelData.target
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: modelData.message.length > 0
                        color: modelData.triggered ? App.colour.bad : App.colour.warn
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        text: modelData.message
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: App.colour.border
            visible: controller && controller.history.length > 0
        }

        // ---- what happened ---------------------------------------------------

        RowLayout {
            Layout.fillWidth: true
            visible: controller && controller.history.length > 0
            Label {
                text: "History"
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Clear"
                flat: true
                onClicked: controller.clearHistory()
            }
        }

        ListView {
            objectName: "alertHistoryList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controller && controller.history.length > 0
            clip: true
            model: controller ? controller.history : []

            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 12

                Rectangle {
                    width: 6
                    height: 6
                    radius: 3
                    color: view.colorForState(modelData.state)
                }
                Label {
                    text: modelData.at
                    color: App.colour.textMuted
                    font.family: App.monospaceFont
                    font.pixelSize: 11
                }
                Label {
                    text: modelData.label
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 240
                    font.pixelSize: 11
                }
                Label {
                    text: modelData.stateText
                    color: view.colorForState(modelData.state)
                    font.pixelSize: 11
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.message.length > 0 ? modelData.message : modelData.value
                    color: modelData.bad ? App.colour.bad : App.colour.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: 11
                }
            }
        }
    }
}
