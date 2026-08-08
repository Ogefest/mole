import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tab body for the live search feature.
Item {
    id: view

    property var controller: null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 8
            rowSpacing: 6

            Label { text: "Search in"; color: "#8b93a7"; font.pixelSize: 12 }
            TextField {
                Layout.fillWidth: true
                Layout.columnSpan: 3
                text: controller ? controller.rootUri : ""
                selectByMouse: true
                font.pixelSize: 12
                onEditingFinished: if (controller) controller.rootUri = text
            }

            Label { text: "Name contains"; color: "#8b93a7"; font.pixelSize: 12 }
            TextField {
                Layout.fillWidth: true
                text: controller ? controller.queryText : ""
                selectByMouse: true
                font.pixelSize: 12
                onTextChanged: if (controller) controller.queryText = text
                onAccepted: if (controller) controller.start()
            }

            Label { text: "Extension"; color: "#8b93a7"; font.pixelSize: 12 }
            TextField {
                Layout.preferredWidth: 120
                placeholderText: "pdf"
                text: controller ? controller.extension : ""
                font.pixelSize: 12
                onTextChanged: if (controller) controller.extension = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: controller && controller.running ? "Stop" : "Search"
                highlighted: true
                onClicked: {
                    if (!controller)
                        return
                    controller.running ? controller.stop() : controller.start()
                }
            }

            CheckBox {
                text: "Case sensitive"
                font.pixelSize: 12
                checked: controller ? controller.caseSensitive : false
                onToggled: if (controller) controller.caseSensitive = checked
            }

            BusyIndicator {
                running: controller ? controller.running : false
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }

            Label {
                Layout.fillWidth: true
                text: controller ? controller.statusText : ""
                color: "#8b93a7"
                elide: Text.ElideRight
                font.pixelSize: 12
            }
        }

        Label {
            Layout.fillWidth: true
            visible: controller ? controller.truncated : false
            text: "Result limit reached — this list is incomplete."
            color: Material.color(Material.Amber)
            font.pixelSize: 12
        }

        SearchResultList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            resultsModel: controller ? controller.results : null
        }
    }
}
