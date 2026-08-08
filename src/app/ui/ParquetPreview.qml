import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A Parquet file as a grid. One dataset, so there is no table list to make.
//
// Read in place: Parquet stores its rows in groups, so a window only decodes the
// groups it touches and a multi-gigabyte file opens at once.
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
                    objectName: "parquetSummary"
                    text: (controller && controller.summary) ? controller.summary : ""
                    color: "#8b93a7"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    Layout.maximumWidth: 520
                }

                Item { Layout.fillWidth: true }

                // Parquet has no query engine behind it, so filtering means
                // scanning. Bounded, and the label says so rather than letting
                // an incomplete count look authoritative.
                TextField {
                    objectName: "parquetFilter"
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

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            visible: view.table && view.table.filter.length > 0
            text: "Filtering scans the file from the start; a very large one is only searched " +
                  "part of the way through."
            color: "#8b93a7"
            wrapMode: Text.Wrap
            font.pixelSize: 10
        }

        DataGrid {
            id: dataGrid
            objectName: "parquetGrid"
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
