import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every saved report, ready to open.
//
// Folders on the left, that folder's runs on the right. A report is worth far
// more as a series than as a snapshot, and a series is only useful if you can
// find it — which was the problem this solves.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "Reports"
                font.pixelSize: 20
                font.bold: true
            }

            Label {
                visible: controller && controller.folderCount > 0
                color: view.mutedColor
                text: controller
                      ? controller.folderCount + " folders · " + controller.reportCount
                        + " runs · " + controller.totalSizeText + " reported"
                      : ""
            }

            Item { Layout.fillWidth: true }

            TextField {
                objectName: "reportFilter"
                Layout.preferredWidth: 240
                visible: controller && controller.folderCount > 0
                font.pixelSize: 12
                placeholderText: "Filter folders…"
                text: controller ? controller.filter : ""
                onTextEdited: if (controller) controller.filter = text
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !controller || controller.folderCount === 0
            color: view.mutedColor
            wrapMode: Text.WordWrap
            text: "No reports have been saved yet.\n\n" +
                  "Analyse a folder — Tools ▸ Analyse folder, or Ctrl+Shift+A — and the run is " +
                  "kept here. Run it again later and the two can be compared, which is where " +
                  "the value is: one report says how big a folder is, a series says what is " +
                  "happening to it.\n\n" +
                  "Put a report on a repeat and the series builds itself."
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controller && controller.folderCount > 0
            spacing: 12

            // ---- the folders ------------------------------------------------

            Rectangle {
                Layout.preferredWidth: Math.max(280, view.width * 0.34)
                Layout.fillHeight: true
                radius: 6
                color: view.panelColor
                border.width: 1
                border.color: view.lineColor

                ListView {
                    objectName: "reportFolderList"
                    anchors.fill: parent
                    anchors.margins: 6
                    clip: true
                    spacing: 2
                    model: controller ? controller.folders : []

                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: folderBody.implicitHeight + 12
                        radius: 4
                        color: modelData.selected ? "#26303f"
                             : folderMouse.containsMouse ? "#20262f" : "transparent"

                        MouseArea {
                            id: folderMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: controller.selectedRoot = modelData.rootUri
                            onDoubleClicked: App.openReportFor(modelData.rootUri)
                        }

                        ColumnLayout {
                            id: folderBody
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    font.bold: true
                                    font.pixelSize: 13
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.runCount === 1
                                          ? "1 run" : modelData.runCount + " runs"
                                    color: view.mutedColor
                                    font.pixelSize: 10
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.rootUri
                                color: view.mutedColor
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: modelData.sizeText + " · " + modelData.fileCountText
                                      + " files · " + modelData.latestText
                                color: view.mutedColor
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            // ---- that folder's runs -----------------------------------------

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: controller ? controller.selectedRoot : ""
                        color: view.mutedColor
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }
                    Button {
                        objectName: "openReportButton"
                        text: "Open latest"
                        enabled: controller && controller.selectedRoot.length > 0
                        // The saved report, not a fresh scan.
                        onClicked: App.openReportFor(controller.selectedRoot)
                    }
                    Button {
                        text: "Forget all"
                        flat: true
                        enabled: controller && controller.selectedRoot.length > 0
                        onClicked: forgetDialog.open()
                    }
                }

                ListView {
                    objectName: "reportRunList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: controller ? controller.runs : []

                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: 30
                        radius: 3
                        color: runMouse.containsMouse ? "#20262f" : "transparent"

                        MouseArea {
                            id: runMouse
                            anchors.fill: parent
                            hoverEnabled: true
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 12

                            Label {
                                text: modelData.takenAt
                                font.family: App.monospaceFont
                                font.pixelSize: 11
                                color: "#c9d1e0"
                            }
                            Label {
                                text: modelData.sizeText
                                font.pixelSize: 11
                                Layout.preferredWidth: 90
                            }
                            Label {
                                text: modelData.fileCountText + " files"
                                color: view.mutedColor
                                font.pixelSize: 11
                                Layout.preferredWidth: 110
                            }
                            Label {
                                text: modelData.changeText
                                color: modelData.changeText.length === 0 ? view.mutedColor
                                     : modelData.grew ? "#d9a441" : "#57ab5a"
                                font.pixelSize: 11
                            }

                            Item { Layout.fillWidth: true }

                            Label {
                                text: modelData.whenText
                                color: view.mutedColor
                                font.pixelSize: 10
                            }
                            ToolButton {
                                text: "×"
                                visible: runMouse.containsMouse
                                implicitWidth: 22
                                implicitHeight: 22
                                onClicked: controller.removeRun(modelData.rootUri, modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: forgetDialog
        anchors.centerIn: parent
        modal: true
        title: "Forget these reports?"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: controller.forgetFolder(controller.selectedRoot)

        Label {
            width: 380
            wrapMode: Text.Wrap
            text: "Every saved run for this folder is deleted. The folder itself is untouched, " +
                  "but the history it could be compared against is gone."
        }
    }
}
