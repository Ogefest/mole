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
                    color: view.mutedColor
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
                color: view.mutedColor
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
                    color: view.panelColor
                    border.width: 1
                    border.color: view.lineColor

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
                            color: modelData.current ? "#26303f"
                                 : setMouse.containsMouse ? "#20262f" : "transparent"

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
                                        text: modelData.count + " items"
                                              + (modelData.driveCount > 1
                                                 ? " · " + modelData.driveCount + " drives" : "")
                                        color: view.mutedColor
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
                        color: view.mutedColor
                        wrapMode: Text.WordWrap
                        text: "This set is empty. Select files in a browser tab and use " +
                              "Tools ▸ Add to set."
                    }

                    ListView {
                        objectName: "setMemberList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: controller ? controller.members : []

                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: 30
                            color: memberMouse.containsMouse ? "#20262f" : "transparent"

                            MouseArea {
                                id: memberMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onDoubleClicked: App.goTo(modelData.location)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 10

                                Label {
                                    text: modelData.missing ? "✕" : (modelData.checked ? "✓" : "·")
                                    color: modelData.missing ? "#e5534b"
                                         : modelData.checked ? "#57ab5a" : "#4a5364"
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    color: modelData.missing ? "#e5534b" : "#d5dbe6"
                                    Layout.preferredWidth: 220
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.location
                                    color: view.mutedColor
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.sizeText
                                    color: view.mutedColor
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
