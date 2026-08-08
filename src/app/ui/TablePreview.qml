import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A delimited file as a grid.
//
// The file is imported into a scratch database rather than parsed into memory,
// so there is no row cap: the view scrolls the whole file and the filter
// searches all of it, not the part that happened to be loaded.
Item {
    id: view
    property var controller: null

    readonly property var table: (controller && controller.table) ? controller.table : null

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- how the file is being read ------------------------------------

        ToolBar {
            Layout.fillWidth: true
            Material.background: "#1b2029"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    text: "Separator"
                    color: "#8b93a7"
                    font.pixelSize: 11
                }
                ComboBox {
                    objectName: "separatorPicker"
                    implicitContentWidthPolicy: ComboBox.WidestText
                    font.pixelSize: 12
                    focusPolicy: Qt.NoFocus
                    model: (controller && controller.separatorChoices) ? controller.separatorChoices : []
                    currentIndex: controller ? model.indexOf(controller.separator) : 0
                    onActivated: if (controller) controller.separator = currentText
                }

                CheckBox {
                    objectName: "headerToggle"
                    text: "First row is a header"
                    font.pixelSize: 12
                    focusPolicy: Qt.NoFocus
                    checked: controller ? controller.firstRowIsHeader === true : true
                    onToggled: if (controller) controller.firstRowIsHeader = checked
                }

                // Filtering runs as SQL over the whole file, so it finds rows
                // far past anything the view has scrolled to.
                TextField {
                    objectName: "tableFilter"
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

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.importing === true : false
                    visible: running
                    implicitWidth: 16
                    implicitHeight: 16
                }

                Label {
                    objectName: "tableSummary"
                    text: (controller && controller.summary) ? controller.summary : ""
                    color: "#8b93a7"
                    font.pixelSize: 11
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

        DataGrid {
            id: dataGrid
            objectName: "csvGrid"
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
