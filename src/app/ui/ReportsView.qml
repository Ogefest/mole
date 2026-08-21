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
                color: App.colour.textMuted
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
            color: App.colour.textMuted
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
                color: App.colour.panel
                border.width: 1
                border.color: App.colour.border

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
                        color: modelData.selected ? App.colour.selection
                             : folderMouse.containsMouse ? App.colour.hover : "transparent"

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
                                    color: App.colour.textMuted
                                    font.pixelSize: 10
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.rootUri
                                color: App.colour.textMuted
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: modelData.sizeText + " · " + modelData.fileCountText
                                      + " files · " + modelData.latestText
                                color: App.colour.textMuted
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
                        color: App.colour.textMuted
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
                        color: runMouse.containsMouse ? App.colour.hover : "transparent"

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
                                color: App.colour.textSecondary
                            }
                            Label {
                                text: modelData.sizeText
                                font.pixelSize: 11
                                Layout.preferredWidth: 90
                            }
                            Label {
                                text: modelData.fileCountText + " files"
                                color: App.colour.textMuted
                                font.pixelSize: 11
                                Layout.preferredWidth: 110
                            }
                            Label {
                                text: modelData.changeText
                                color: modelData.changeText.length === 0 ? App.colour.textMuted
                                     : modelData.grew ? App.colour.warn : App.colour.ok
                                font.pixelSize: 11
                            }

                            Item { Layout.fillWidth: true }

                            Label {
                                text: modelData.whenText
                                color: App.colour.textMuted
                                font.pixelSize: 10
                            }
                            ToolButton {
                                text: "×"
                                font.pixelSize: App.textSize
                                visible: runMouse.containsMouse
                                implicitWidth: App.minimumTarget
                                implicitHeight: App.minimumTarget
                                onClicked: controller.removeRun(modelData.rootUri, modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        // A dialog sits on the panel ground, said here rather than inherited:
        // the window no longer hands one down. See ADR-0074.
        Material.background: App.colour.panel
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: forgetDialog
        objectName: "forgetDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        focus: true
        anchors.centerIn: parent
        modal: true
        title: "Forget these reports?"

        // History that cannot be recovered, offered until now on a button
        // labelled "Ok" in the same grey as the one beside it.
        footer: ConfirmButtons {
            acceptText: "Forget"
            destructive: true
        }

        onAccepted: controller.forgetFolder(controller.selectedRoot)

        Label {
            width: 380
            wrapMode: Text.Wrap
            text: "Every saved run for this folder is deleted. The folder itself is untouched, " +
                  "but the history it could be compared against is gone."
        }
    }
}
