import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A file of JSON records as a grid.
//
// TablePreview.qml without the separator picker and the header checkbox: a
// record has neither a separator to guess nor a first row that might be a
// header, so there is nothing to choose about how the file is read. What is
// left is the summary, the filter, Copy and the grid -- and paging, selection
// and copying come with the grid.
//
// The source, when the records are not JSON objects or somebody asked for it,
// is the text viewer hosted here rather than a second one written out. See
// JsonLinesPreviewController.
Item {
    id: view
    property var controller: null

    readonly property var table: (controller && controller.table) ? controller.table : null
    readonly property bool showsSource: controller ? controller.showingSource === true : false

    // The same one-second threshold as the delimited grid, and the same reason:
    // records appear as the import commits them, so what is left to cover is the
    // gap before the first batch, and on a slow drive an empty grid during it
    // reads as "this file is empty".
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

    // The source, built by the controller only once something needs it, so the
    // reader who never asks for it costs nothing.
    Loader {
        anchors.fill: parent
        active: view.showsSource
        source: active ? "TextPreview.qml" : ""
        onLoaded: if (item) item.controller = controller ? controller.source : null
    }

    Connections {
        target: controller
        function onImportProgress() { /* rebinds the loader through showsSource */ }
        function onLoadingChanged() { view.watchTheImport() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !view.showsSource

        ToolBar {
            Layout.fillWidth: true
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                // Filtering runs as SQL over the whole file, so it finds records
                // far past anything the view has scrolled to -- and because a
                // nested value is JSON in its cell, it finds what is inside one.
                TextField {
                    objectName: "recordsFilter"
                    Layout.preferredWidth: 220
                    font.pixelSize: App.secondaryTextSize
                    placeholderText: "Filter records…"
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
                    objectName: "recordsSummary"
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

        Timer {
            id: slowImport
            interval: 1000
            repeat: false
            property bool tripped: false
            onTriggered: tripped = true
        }

        // Anchored inside a filling Item rather than laid out: a ColumnLayout is
        // only as wide as its widest child, so centring in one puts this against
        // the left edge instead of in the middle.
        Item {
            objectName: "recordsLoadingView"
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
                    text: "Records appear as they are read, and the first ones are on their way. "
                          + "Nothing is stuck."
                    color: App.colour.textFaint
                    font.pixelSize: App.smallTextSize
                }
            }
        }

        DataGrid {
            id: dataGrid
            objectName: "recordsGrid"
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
