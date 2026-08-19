import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A sync between two folders, on any two drives.
//
// The dry run is the default and Preview is the prominent button. A mirror is
// the one thing here that deletes files nobody asked it to touch, and finding
// that out afterwards is finding it out too late.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"
    readonly property color warnColor: "#d9a441"
    readonly property color badColor: "#e5534b"

    function focusActivePane() { body.forceActiveFocus() }

    FocusScope {
        id: body
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: "Sync"
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    objectName: "syncSummary"
                    text: controller ? controller.planSummary : ""
                    color: controller && controller.deleteCount > 0 ? view.warnColor
                                                                    : view.mutedColor
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.running : false
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                Button {
                    objectName: "previewButton"
                    text: "Preview"
                    highlighted: true
                    enabled: controller && controller.ready && !controller.running
                    onClicked: controller.preview()
                    ToolTip.visible: hovered
                    ToolTip.text: "Work out exactly what would happen, and write nothing"
                }
                Button {
                    objectName: "applyButton"
                    text: "Apply"
                    enabled: controller && controller.hasPlan && !controller.running
                    onClicked: controller.deleteCount > 0 ? confirmDelete.open() : controller.apply()
                }
                Button {
                    text: "Stop"
                    visible: controller && controller.running
                    onClicked: controller.cancel()
                }
            }

            // ---- the two ends ----------------------------------------------

            Rectangle {
                Layout.fillWidth: true
                radius: 6
                color: view.panelColor
                border.width: 1
                border.color: view.lineColor
                implicitHeight: ends.implicitHeight + 22

                GridLayout {
                    id: ends
                    anchors.fill: parent
                    anchors.margins: 11
                    columns: 3
                    columnSpacing: 8
                    rowSpacing: 6

                    Label {
                        text: "From"
                        color: view.mutedColor
                        font.pixelSize: 12
                    }
                    TextField {
                        objectName: "syncSource"
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        placeholderText: "Source folder"
                        text: controller ? controller.sourceUri : ""
                        onEditingFinished: if (controller) controller.sourceUri = text
                    }
                    ToolButton {
                        text: "⇅"
                        onClicked: controller.swapEnds()
                        ToolTip.visible: hovered
                        ToolTip.text: "Swap the two ends"
                    }

                    Label {
                        text: "To"
                        color: view.mutedColor
                        font.pixelSize: 12
                    }
                    TextField {
                        objectName: "syncTarget"
                        Layout.fillWidth: true
                        Layout.columnSpan: 2
                        font.pixelSize: 12
                        placeholderText: "Destination folder"
                        text: controller ? controller.targetUri : ""
                        onEditingFinished: if (controller) controller.targetUri = text
                    }
                }
            }

            // ---- how -------------------------------------------------------

            Rectangle {
                Layout.fillWidth: true
                radius: 6
                color: view.panelColor
                border.width: 1
                border.color: view.lineColor
                implicitHeight: options.implicitHeight + 22

                ColumnLayout {
                    id: options
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Picker {
                            objectName: "syncMode"
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "id"
                            model: controller ? controller.modes : []
                            currentIndex: {
                                if (!controller)
                                    return 0
                                const all = controller.modes
                                for (let i = 0; i < all.length; ++i) {
                                    if (all[i].id === controller.mode)
                                        return i
                                }
                                return 0
                            }
                            onActivated: controller.mode = currentValue
                        }

                        Label {
                            text: "Changed when"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        Picker {
                            objectName: "syncCompare"
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "id"
                            model: controller ? controller.compareChoices : []
                            currentIndex: {
                                if (!controller)
                                    return 0
                                const all = controller.compareChoices
                                for (let i = 0; i < all.length; ++i) {
                                    if (all[i].id === controller.compare)
                                        return i
                                }
                                return 0
                            }
                            onActivated: controller.compare = currentValue
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: controller ? controller.modeDescription : ""
                        color: "#6f7788"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        CheckBox {
                            objectName: "syncSkipNewer"
                            text: "Never overwrite something newer"
                            font.pixelSize: 12
                            checked: controller ? controller.skipNewer : true
                            onToggled: controller.skipNewer = checked
                        }
                        CheckBox {
                            text: "Subfolders"
                            font.pixelSize: 12
                            checked: controller ? controller.recursive : true
                            onToggled: controller.recursive = checked
                        }
                        CheckBox {
                            text: "Hidden files"
                            font.pixelSize: 12
                            checked: controller ? controller.includeHidden : false
                            onToggled: controller.includeHidden = checked
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: "Only"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        TextField {
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            placeholderText: "*.jpg;*.raw   (blank means everything)"
                            text: controller ? controller.includePatterns : ""
                            onEditingFinished: controller.includePatterns = text
                        }
                        Label {
                            text: "Except"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        TextField {
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            placeholderText: "*.tmp;.git"
                            text: controller ? controller.excludePatterns : ""
                            onEditingFinished: controller.excludePatterns = text
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: controller && controller.errorText.length > 0
                text: controller ? controller.errorText : ""
                color: view.badColor
                wrapMode: Text.WordWrap
                font.pixelSize: 12
            }

            Label {
                Layout.fillWidth: true
                visible: controller && controller.running && controller.progressText.length > 0
                text: controller ? controller.progressText : ""
                color: view.mutedColor
                elide: Text.ElideMiddle
                font.pixelSize: 11
            }

            // ---- what would happen ------------------------------------------

            Label {
                Layout.fillWidth: true
                visible: !controller || !controller.hasPlan
                color: view.mutedColor
                wrapMode: Text.WordWrap
                text: "Preview first. It works out exactly what would happen — every copy, "
                      + "replacement and deletion, with a reason for each — and writes nothing. "
                      + "Apply then carries out that same plan."
            }

            ListView {
                objectName: "syncStepList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: controller && controller.hasPlan
                clip: true
                model: controller ? controller.steps : []

                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    spacing: 10

                    Label {
                        Layout.preferredWidth: 80
                        text: modelData.action
                        font.pixelSize: 11
                        color: modelData.action === "delete" ? view.badColor
                             : modelData.destructive ? view.warnColor
                             : modelData.skipped ? "#5c6472" : "#57ab5a"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.path
                        font.family: App.monospaceFont
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        color: modelData.skipped ? "#5c6472" : "#d5dbe6"
                    }
                    Label {
                        Layout.preferredWidth: 90
                        horizontalAlignment: Text.AlignRight
                        text: modelData.sizeText
                        color: view.mutedColor
                        font.pixelSize: 11
                    }
                    Label {
                        Layout.preferredWidth: 200
                        text: modelData.reason
                        color: view.mutedColor
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    Dialog {
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: confirmDelete
        objectName: "confirmSyncDelete"
        anchors.centerIn: parent
        modal: true
        title: "This will delete files"
        focus: true
        footer: ConfirmButtons {
            acceptText: "Delete and sync"
            rejectText: "Cancel"
            destructive: true
        }
        width: 520

        property var doomed: []

        onAboutToShow: doomed = controller ? controller.deletions() : []
        onAccepted: controller.apply()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: App.textSize
                text: controller
                      ? controller.deleteCount + " files at the destination are not in the source "
                        + "and will be removed:"
                      : ""
            }
            TargetList {
                objectName: "syncDeleteList"
                Layout.fillWidth: true
                model: confirmDelete.doomed
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "This cannot be undone."
                color: "#d9a441"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
