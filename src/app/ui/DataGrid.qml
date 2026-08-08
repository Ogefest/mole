import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A table with selectable, copyable cells and a filter.
//
// Shared by every tabular viewer -- delimited text, SQLite, Parquet -- because
// none of the behaviour here is specific to where the rows came from. The
// alternative was three copies of the selection arithmetic, which is three
// places for it to drift.
Item {
    id: grid

    /// A TableModel. Whatever fills it is the caller's business.
    property var table: null
    /// Called with (top, left, bottom, right) when the user asks for a copy.
    /// The controller owns the clipboard; this only knows what is selected.
    signal copyRequested(int top, int left, int bottom, int right)

    // The selected block, in cell coordinates. Anchor is where the drag began,
    // cursor is where it is now, so dragging backwards works like dragging
    // forwards.
    property int anchorRow: -1
    property int anchorColumn: -1
    property int cursorRow: -1
    property int cursorColumn: -1

    readonly property int selectionTop: Math.min(anchorRow, cursorRow)
    readonly property int selectionBottom: Math.max(anchorRow, cursorRow)
    readonly property int selectionLeft: Math.min(anchorColumn, cursorColumn)
    readonly property int selectionRight: Math.max(anchorColumn, cursorColumn)
    readonly property bool hasSelection: anchorRow >= 0 && cursorRow >= 0

    function isSelected(row, column) {
        return hasSelection && row >= selectionTop && row <= selectionBottom
               && column >= selectionLeft && column <= selectionRight
    }

    function selectCell(row, column) {
        anchorRow = cursorRow = row
        anchorColumn = cursorColumn = column
    }

    function extendTo(row, column) {
        if (anchorRow < 0)
            selectCell(row, column)
        cursorRow = row
        cursorColumn = column
    }

    function clearSelection() {
        anchorRow = cursorRow = anchorColumn = cursorColumn = -1
    }

    function copySelection() {
        if (hasSelection)
            grid.copyRequested(selectionTop, selectionLeft, selectionBottom, selectionRight)
    }

    function moveCursor(rowStep, columnStep, extend) {
        if (!table || cursorRow < 0)
            return
        const row = Math.max(0, Math.min(table.rows - 1, cursorRow + rowStep))
        const column = Math.max(0, Math.min(table.columns - 1, cursorColumn + columnStep))
        if (extend)
            extendTo(row, column)
        else
            selectCell(row, column)
        view.positionViewAtCell(Qt.point(column, row), TableView.Contain)
    }

    // Character width for the current font, used to turn the source's width
    // hints into pixels. Measured once rather than guessed.
    TextMetrics {
        id: metrics
        font.family: App.monospaceFont
        font.pixelSize: 12
        text: "0"
    }

    // Columns are sized to what is actually in them. The header row and the grid
    // share this rather than asking the TableView, which reports -1 for a column
    // it has not laid out yet -- and the header is built before the first pass.
    function columnPixels(column) {
        const hints = table ? table.columnWidths : []
        const characters = column < hints.length ? hints[column] : 12
        const padded = Math.min(Math.max(characters, 4), 60) + 2
        return Math.round(padded * metrics.advanceWidth)
    }

    Keys.onPressed: function(event) {
        if (event.matches(StandardKey.Copy)) {
            copySelection()
            event.accepted = true
            return
        }
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        switch (event.key) {
        case Qt.Key_Up:    moveCursor(-1, 0, extend); event.accepted = true; break
        case Qt.Key_Down:  moveCursor(1, 0, extend);  event.accepted = true; break
        case Qt.Key_Left:  moveCursor(0, -1, extend); event.accepted = true; break
        case Qt.Key_Right: moveCursor(0, 1, extend);  event.accepted = true; break
        case Qt.Key_PageDown: moveCursor(25, 0, extend); event.accepted = true; break
        case Qt.Key_PageUp:   moveCursor(-25, 0, extend); event.accepted = true; break
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Headers scroll with the grid but never leave the top.
        Item {
            Layout.fillWidth: true
            implicitHeight: 26
            clip: true

            Row {
                x: -view.contentX
                height: parent.height

                Connections {
                    target: grid.table ? grid.table : null
                    function onTableChanged() { view.forceLayout() }
                }

                Repeater {
                    model: grid.table ? grid.table.columns : 0
                    delegate: Rectangle {
                        required property int index
                        width: grid.columnPixels(index)
                        height: 26
                        color: "#20262f"
                        border.width: 1
                        border.color: "#2a3140"

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            verticalAlignment: Text.AlignVCenter
                            text: grid.table ? grid.table.headerAt(index) : ""
                            elide: Text.ElideRight
                            font.pixelSize: 11
                            font.bold: true
                            color: "#c9d1e0"
                        }
                    }
                }
            }
        }

        TableView {
            id: view
            objectName: "tableGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: grid.table
            boundsBehavior: Flickable.StopAtBounds

            columnWidthProvider: function(column) { return grid.columnPixels(column) }
            rowHeightProvider: function(row) { return 22 }

            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            delegate: Rectangle {
                required property int row
                required property int column
                required property string cell

                implicitWidth: 100
                implicitHeight: 22
                color: grid.isSelected(row, column) ? "#2f4a6b"
                     : (row % 2 === 0 ? "#161a21" : "#1b2029")
                border.width: grid.cursorRow === row && grid.cursorColumn === column ? 1 : 0
                border.color: Material.accent

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    verticalAlignment: Text.AlignVCenter
                    text: cell
                    elide: Text.ElideRight
                    font.family: App.monospaceFont
                    font.pixelSize: 12
                    // A null reads differently from an empty string, because the
                    // difference is usually what the reader is looking for.
                    color: cell === "NULL" ? "#6f7788" : "#d5dbe6"
                    font.italic: cell === "NULL"
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: function(mouse) {
                        grid.forceActiveFocus()
                        if (mouse.modifiers & Qt.ShiftModifier)
                            grid.extendTo(row, column)
                        else
                            grid.selectCell(row, column)
                    }
                    // Dragging across the grid extends the block, the way it
                    // does in a spreadsheet.
                    onPositionChanged: function(mouse) {
                        if (pressed)
                            grid.extendTo(row, column)
                    }
                }
            }
        }

        // ---- what is selected ------------------------------------------------

        ToolBar {
            Layout.fillWidth: true
            visible: grid.hasSelection
            Material.background: "#1b2029"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8

                Label {
                    Layout.fillWidth: true
                    color: "#8b93a7"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    text: {
                        if (!grid.hasSelection || !grid.table)
                            return ""
                        const cells = (grid.selectionBottom - grid.selectionTop + 1)
                                    * (grid.selectionRight - grid.selectionLeft + 1)
                        if (cells === 1)
                            return grid.table.headerAt(grid.selectionLeft) + ":  "
                                   + grid.table.cellAt(grid.selectionTop, grid.selectionLeft)
                        return cells + " cells selected  ·  Ctrl+C to copy"
                    }
                }
            }
        }
    }
}
