import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A SQLite file: its tables on the left, the selected one as a grid.
//
// Read in place and read-only. The database is already a queryable table, so
// there is nothing to import -- which is why a file of any size opens at once.
Item {
    id: view
    property var controller: null

    readonly property var table: (controller && controller.table) ? controller.table : null

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            Material.background: "#1b2029"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    objectName: "sqliteSummary"
                    text: (controller && controller.summary) ? controller.summary : ""
                    color: "#8b93a7"
                    font.pixelSize: 11
                }

                Item { Layout.fillWidth: true }

                // Searches the selected table as SQL, so it reaches rows far
                // past anything the view has scrolled to.
                TextField {
                    objectName: "sqliteFilter"
                    Layout.preferredWidth: 220
                    font.pixelSize: 12
                    placeholderText: "Filter rows…"
                    text: view.table ? view.table.filter : ""
                    onTextEdited: if (view.table) view.table.filter = text
                    Keys.onEscapePressed: {
                        text = ""
                        if (view.table) view.table.filter = ""
                    }
                }

                ToolButton {
                    text: "Copy"
                    enabled: dataGrid.hasSelection
                    font.pixelSize: 12
                    focusPolicy: Qt.NoFocus
                    onClicked: dataGrid.copySelection()
                    ToolTip.text: "Copy the selected cells  (Ctrl+C)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.margins: 8
            visible: controller && controller.errorText.length > 0
            text: controller ? controller.errorText : ""
            color: Material.color(Material.Red)
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The table list. Narrow, because a database usually has a handful
            // of tables with short names and the grid needs the room.
            Rectangle {
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                visible: controller && controller.tables.length > 0
                color: "#1b2029"

                ListView {
                    objectName: "sqliteTableList"
                    anchors.fill: parent
                    anchors.margins: 4
                    clip: true
                    spacing: 1
                    model: controller ? controller.tables : []

                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: 26
                        radius: 3
                        color: modelData.current ? "#26303f"
                             : tableMouse.containsMouse ? "#20262f" : "transparent"

                        MouseArea {
                            id: tableMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: controller.currentTable = modelData.name
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: modelData.name
                                elide: Text.ElideMiddle
                                font.pixelSize: 12
                                font.bold: modelData.current
                                color: "#d5dbe6"
                            }
                            Label {
                                text: modelData.rowsText
                                color: "#6f7788"
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            DataGrid {
                id: dataGrid
                objectName: "sqliteGrid"
                Layout.fillWidth: true
                Layout.fillHeight: true
                table: view.table
                onCopyRequested: function(top, left, bottom, right) {
                    if (controller)
                        controller.copyBlock(top, left, bottom, right)
                }
            }
        }
    }
}
