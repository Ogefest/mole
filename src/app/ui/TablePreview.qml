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

    // Called on every way into the waiting state, including the one that is easy
    // to miss: the controller starts reading before this view is instantiated, so
    // by the time there is anything here to react to, `loadingChanged` has
    // already been and gone.
    function watchTheImport() {
        if (controller && controller.loading) {
            slowImport.tripped = false
            slowImport.restart()
        } else {
            slowImport.stop()
            slowImport.tripped = false
        }
    }

    Component.onCompleted: watchTheImport()
    onControllerChanged: watchTheImport()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- how the file is being read ------------------------------------

        ToolBar {
            Layout.fillWidth: true
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    text: "Separator"
                    color: App.colour.textMuted
                    font.pixelSize: App.smallTextSize
                }
                Picker {
                    objectName: "separatorPicker"
                    font.pixelSize: App.secondaryTextSize
                    focusPolicy: Qt.NoFocus
                    model: (controller && controller.separatorChoices) ? controller.separatorChoices : []
                    currentIndex: controller ? model.indexOf(controller.separator) : 0
                    onActivated: if (controller) controller.separator = currentText
                }

                CheckBox {
                    objectName: "headerToggle"
                    text: "First row is a header"
                    font.pixelSize: App.secondaryTextSize
                    focusPolicy: Qt.NoFocus
                    checked: controller ? controller.firstRowIsHeader === true : true
                    onToggled: if (controller) controller.firstRowIsHeader = checked
                }

                // Filtering runs as SQL over the whole file, so it finds rows
                // far past anything the view has scrolled to.
                TextField {
                    objectName: "tableFilter"
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
                    color: App.colour.textMuted
                    font.pixelSize: App.smallTextSize
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

        // Rows appear as the import commits them, so for most files there is
        // nothing to wait for. What is left is the gap before the first batch --
        // on a slow drive that gap is long, and an empty grid during it reads as
        // "this file is empty" rather than "still reading". One second is the
        // threshold, as in the file pane: below it a spinner is only a flash.
        Timer {
            id: slowImport
            interval: 1000
            repeat: false
            property bool tripped: false
            onTriggered: tripped = true
        }

        Connections {
            target: controller
            function onLoadingChanged() { view.watchTheImport() }
        }

        // Anchored inside a filling Item rather than laid out: a ColumnLayout is
        // only as wide as its widest child, so centring in one puts this against
        // the left edge instead of in the middle.
        Item {
            objectName: "csvLoadingView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: slowImport.tripped && controller && controller.importing === true
                     && (view.table ? view.table.rows === 0 : true)

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 420)
                spacing: 10

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: parent.parent.visible
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Reading this file…"
                    font.pixelSize: App.textSize
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: "Rows appear as they are read, and the first ones are on their way. "
                          + "Nothing is stuck."
                    color: App.colour.textFaint
                    font.pixelSize: App.smallTextSize
                }
            }
        }

        DataGrid {
            id: dataGrid
            objectName: "csvGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !(slowImport.tripped && controller && controller.importing === true
                       && (view.table ? view.table.rows === 0 : true))
            table: view.table
            onCopyRequested: function(top, left, bottom, right) {
                if (controller)
                    controller.copyBlock(top, left, bottom, right)
            }
        }
    }
}
