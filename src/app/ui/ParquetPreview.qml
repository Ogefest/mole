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
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    objectName: "parquetSummary"
                    text: (controller && controller.summary) ? controller.summary : ""
                    color: App.colour.textMuted
                    font.pixelSize: App.smallTextSize
                    elide: Text.ElideRight
                    Layout.maximumWidth: 520
                }

                // A row of the grid is read on a task now -- a row group written as
                // the whole file is not something to decode on the thread that
                // draws -- so the rows arrive a moment after the file opens. Said
                // out loud: a grid filling in must not read as a grid with holes.
                BusyIndicator {
                    running: view.table ? view.table.reading : false
                    visible: running
                    implicitWidth: 16
                    implicitHeight: 16
                }

                Label {
                    objectName: "parquetReading"
                    visible: view.table && view.table.reading
                    text: "Reading…"
                    color: App.colour.textMuted
                    font.pixelSize: App.smallTextSize
                }

                Item { Layout.fillWidth: true }

                // Parquet has no query engine behind it, so filtering means
                // scanning. Bounded, and the label says so rather than letting
                // an incomplete count look authoritative.
                TextField {
                    objectName: "parquetFilter"
                    Layout.preferredWidth: 220
                    font.pixelSize: App.secondaryTextSize
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
                    font.pixelSize: App.secondaryTextSize
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
            color: App.colour.bad
            wrapMode: Text.Wrap
            font.pixelSize: App.secondaryTextSize
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            visible: view.table && view.table.filter.length > 0
            text: "Filtering scans the file from the start; a very large one is only searched " +
                  "part of the way through."
            color: App.colour.textMuted
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
