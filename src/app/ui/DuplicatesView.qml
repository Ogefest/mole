import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Finding duplicates, and deciding what to do about them.
//
// Choosing what to keep is the hard half, and this never picks for you. It
// offers the choices people actually make — keep the newest, the oldest, the one
// nearest the top of the tree — and says what each would free before anything is
// deleted.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"

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
                    text: "Duplicates"
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    objectName: "duplicateSummary"
                    text: controller ? controller.summary : ""
                    color: view.mutedColor
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.scanning : false
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                Button {
                    objectName: "scanButton"
                    text: controller && controller.scanning ? "Stop" : "Scan"
                    highlighted: !controller || !controller.scanning
                    enabled: controller && controller.roots.length > 0
                    onClicked: controller.scanning ? controller.cancel() : controller.scan()
                }
            }

            // ---- what and how ----------------------------------------------

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

                    Label {
                        Layout.fillWidth: true
                        text: controller && controller.roots.length > 0
                              ? controller.roots.join("\n")
                              : "Open this from a folder to search it."
                        color: view.mutedColor
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            text: "Match by"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        ComboBox {
                            objectName: "strategyPicker"
                            implicitContentWidthPolicy: ComboBox.WidestText
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "id"
                            model: controller ? controller.strategies : []
                            currentIndex: {
                                if (!controller)
                                    return 0
                                const all = controller.strategies
                                for (let i = 0; i < all.length; ++i) {
                                    if (all[i].id === controller.strategyId)
                                        return i
                                }
                                return 0
                            }
                            onActivated: controller.strategyId = currentValue
                        }

                        Label {
                            text: "Ignore below"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        ComboBox {
                            objectName: "minimumSizePicker"
                            implicitContentWidthPolicy: ComboBox.WidestText
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "bytes"
                            model: [
                                { bytes: 1, label: "anything" },
                                { bytes: 1024, label: "1 kB" },
                                { bytes: 102400, label: "100 kB" },
                                { bytes: 1048576, label: "1 MB" },
                                { bytes: 10485760, label: "10 MB" }
                            ]
                            currentIndex: {
                                if (!controller)
                                    return 1
                                for (let i = 0; i < model.length; ++i) {
                                    if (model[i].bytes === controller.minimumSize)
                                        return i
                                }
                                return 1
                            }
                            onActivated: controller.minimumSize = currentValue
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // What this choice costs, not only what it matches. That is
                    // the part someone needs before starting a scan on a NAS.
                    Label {
                        Layout.fillWidth: true
                        text: controller ? controller.strategyDescription : ""
                        color: "#6f7788"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                }
            }

            // ---- what to keep ----------------------------------------------

            RowLayout {
                Layout.fillWidth: true
                visible: controller && controller.groupCount > 0
                spacing: 8

                Label {
                    text: "Keep"
                    color: view.mutedColor
                    font.pixelSize: 12
                }
                Button {
                    text: "Newest"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepNewest()
                }
                Button {
                    text: "Oldest"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepOldest()
                }
                Button {
                    text: "Nearest the top"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepShortestPath()
                    ToolTip.visible: hovered
                    ToolTip.text: "The copy with the shortest path is usually the original"
                }
                Button {
                    text: "Nothing"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.clearSelection()
                }

                Item { Layout.fillWidth: true }

                Label {
                    objectName: "duplicateSelection"
                    visible: controller && controller.selectedCount > 0
                    text: controller
                          ? controller.selectedCount + " ticked · " + controller.selectedSizeText
                          : ""
                    color: "#d9a441"
                    font.pixelSize: 12
                }
                Button {
                    objectName: "deleteDuplicatesButton"
                    text: "Delete ticked"
                    enabled: controller && controller.selectedCount > 0
                    onClicked: confirmDelete.open()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: controller && controller.hasRun && controller.groupCount === 0
                color: view.mutedColor
                wrapMode: Text.WordWrap
                text: "Nothing matched. A different strategy may still find something — " +
                      "'Identical contents' proves files are the same, while 'Same name' " +
                      "finds copies that were edited apart."
            }

            // ---- the groups -------------------------------------------------

            ListView {
                objectName: "duplicateGroupList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: controller && controller.groupCount > 0
                clip: true
                spacing: 8
                model: controller ? controller.groups : []

                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: groupBody.implicitHeight + 18
                    radius: 6
                    color: view.panelColor
                    border.width: 1
                    border.color: view.lineColor

                    ColumnLayout {
                        id: groupBody
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.count + " copies · " + modelData.sizeText + " each"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: modelData.reclaimableText + " could be freed"
                                color: "#d9a441"
                                font.pixelSize: 11
                            }
                        }

                        Repeater {
                            model: modelData.files
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                CheckBox {
                                    checked: modelData.selected
                                    onToggled: controller.toggle(modelData.uri)
                                }
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    font.family: App.monospaceFont
                                    Layout.preferredWidth: 220
                                    elide: Text.ElideMiddle
                                    color: modelData.selected ? "#d9a441" : "#d5dbe6"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.location
                                    color: view.mutedColor
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.modifiedText
                                    color: view.mutedColor
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: confirmDelete
        objectName: "confirmDeleteDuplicates"
        anchors.centerIn: parent
        modal: true
        title: "Delete these files?"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: controller.deleteSelected()

        Label {
            width: 420
            wrapMode: Text.Wrap
            text: controller
                  ? controller.selectedCount + " files, " + controller.selectedSizeText
                    + ". This cannot be undone, and the copies you did not tick are left alone."
                  : ""
        }
    }
}
