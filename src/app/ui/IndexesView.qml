import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every index that exists, and how much any of them can be trusted.
//
// An index is a claim about a tree that goes quietly out of date. The only place
// one used to be visible was a dropdown inside the search form, as a label and a
// count — so how old a search's answer was, and whether it could answer a
// question about a camera at all, was decided by something nobody could look at.
//
// One row per index, stalest first, and every column is a question somebody
// actually has: what it covers, how old it is, how big it is, what kind of scan
// built it, and whether anything is keeping it fresh.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"
    readonly property color warnColor: "#d8a657"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "Indexes"
                font.pixelSize: 20
                font.bold: true
            }

            Label {
                objectName: "indexesSummary"
                visible: controller && controller.volumeCount > 0
                color: view.mutedColor
                text: controller
                      ? controller.volumeCount + (controller.volumeCount === 1 ? " index · " : " indexes · ")
                        + controller.totalEntriesText + " entries · "
                        + (controller.scheduledCount > 0
                           ? controller.scheduledCount + " kept up to date"
                           : "none kept up to date")
                      : ""
            }

            Item { Layout.fillWidth: true }

            TextField {
                objectName: "indexFilter"
                Layout.preferredWidth: 240
                visible: controller && controller.volumeCount > 0
                font.pixelSize: 12
                placeholderText: "Filter indexes…"
                text: controller ? controller.filter : ""
                onTextEdited: if (controller) controller.filter = text
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !controller || controller.volumeCount === 0
            color: view.mutedColor
            wrapMode: Text.WordWrap
            text: "Nothing is indexed yet.\n\n" +
                  "Index a folder — Tools ▸ Index this folder, or the button in a search tab — and " +
                  "searching it afterwards never touches the disk again. That is the difference " +
                  "between instant and a walk of the whole tree on a slow drive.\n\n" +
                  "An index goes out of date on its own, so put one on a clock and it keeps itself " +
                  "current: a nightly run costs a walk of what moved rather than of everything."
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controller && controller.volumeCount > 0
            radius: 6
            color: view.panelColor
            border.width: 1
            border.color: view.lineColor

            ListView {
                objectName: "indexList"
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 2
                model: controller ? controller.volumes : []

                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: rowBody.implicitHeight + 14
                    radius: 4
                    color: rowMouse.containsMouse ? "#20262f" : "transparent"

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        // The tree itself, which is what somebody looking at a
                        // stale index usually wants next.
                        onDoubleClicked: App.openLocation(modelData.rootUri)
                    }

                    ColumnLayout {
                        id: rowBody
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                text: modelData.label
                                font.bold: true
                                font.pixelSize: 13
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: modelData.entryCountText + " entries"
                                color: view.mutedColor
                                font.pixelSize: 10
                            }
                            Item { Layout.fillWidth: true }
                            // Said in its own colour when nothing is keeping it
                            // fresh, because that is the row's real news.
                            Label {
                                objectName: "indexScheduleText"
                                text: modelData.scheduled
                                      ? modelData.scheduleText + " · next " + modelData.nextDueText
                                      : modelData.scheduleText
                                color: modelData.scheduled ? view.mutedColor : view.warnColor
                                font.pixelSize: 11
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
                            Layout.fillWidth: true
                            text: modelData.scannedText + " · " + modelData.kindText
                            color: modelData.kindKnown ? view.mutedColor : view.warnColor
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        // The other half of the answer, and a different question: this tab lists
        // what exists, Automation lists what runs by itself. An index with no
        // rule does not appear there at all, which is why the two are not one.
        RowLayout {
            Layout.fillWidth: true
            visible: controller && controller.volumeCount > 0
            spacing: 8

            Label {
                Layout.fillWidth: true
                color: view.mutedColor
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                text: "An index is only as fresh as its last scan. Put one on a clock where it was " +
                      "made, in a search tab, and it keeps itself current."
            }
            Button {
                objectName: "openAutomationButton"
                text: "Automation"
                flat: true
                onClicked: App.openFeatureTab("core.automation")
            }
        }
    }
}
