import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The bytes of a file, for the files nothing else can show.
//
// Read-only, like every other preview: there is no path from this view that
// writes anything. Three columns of fixed-width text, so every position in the
// row is arithmetic rather than layout -- which is what makes selecting a byte
// range by dragging possible at all.
Item {
    id: view
    property var controller: null

    readonly property int bytesPerRow: 16
    readonly property var rows: controller ? controller.rows : []
    readonly property real cellWidth: metrics.advanceWidth
    readonly property real rowHeight: Math.ceil(metrics.height) + 2

    // One character of the monospaced family, measured rather than assumed: the
    // selection rectangles are placed in character columns.
    TextMetrics {
        id: metrics
        font.family: App.monospaceFont
        font.pixelSize: App.monospaceSize
        text: "0"
    }

    // The two measurements the delegate and the hit test have to agree on, in
    // one place: if they disagree, dragging selects the byte next to the one
    // under the pointer.
    readonly property real gutter: 8
    readonly property real columnGap: 16

    // Where a byte sits in the row, in characters. Two hex digits and a space
    // each, with one more space between the two groups of eight.
    readonly property int gapColumn: (bytesPerRow / 2) * 3
    function hexColumn(index) { return index * 3 + (index >= bytesPerRow / 2 ? 1 : 0) }
    // From the first digit of the first byte to the last digit of the last one,
    // gap included when the run crosses it.
    function hexSpan(from, count) { return hexColumn(from + count - 1) + 2 - hexColumn(from) }
    function byteAtColumn(column) {
        return Math.floor((column >= gapColumn ? column - 1 : column) / 3)
    }

    ViewerKeys {
        id: viewerKeys
        reserved: [[Qt.Key_PageDown, Qt.ControlModifier], [Qt.Key_PageUp, Qt.ControlModifier],
                   [Qt.Key_Home, Qt.ControlModifier], [Qt.Key_End, Qt.ControlModifier],
                   [Qt.Key_C, Qt.ControlModifier],
                   [Qt.Key_C, Qt.ControlModifier | Qt.ShiftModifier]]
    }

    focus: true
    Keys.onPressed: function(event) {
        if (!controller)
            return
        const control = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0

        if (control && event.key === Qt.Key_C) {
            // Both forms, because a header is read as hex and a string inside a
            // binary is read as text.
            if (shift)
                controller.copySelectionAsText()
            else
                controller.copySelectionAsHex()
            event.accepted = true
            return
        }
        if (!controller.paged)
            return
        if (control && event.key === Qt.Key_PageDown) {
            controller.nextWindow(); event.accepted = true
        } else if (control && event.key === Qt.Key_PageUp) {
            controller.previousWindow(); event.accepted = true
        } else if (control && event.key === Qt.Key_Home) {
            controller.firstWindow(); event.accepted = true
        } else if (control && event.key === Qt.Key_End) {
            controller.lastWindow(); event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 4
            spacing: 8

            BusyIndicator {
                running: controller ? controller.loading : false
                visible: running
                implicitWidth: 18
                implicitHeight: 18
            }

            Label {
                objectName: "hexError"
                Layout.fillWidth: true
                visible: controller && controller.errorText.length > 0
                text: controller ? controller.errorText : ""
                color: App.colour.bad
                wrapMode: Text.Wrap
                font.pixelSize: App.secondaryTextSize
            }

            Label {
                objectName: "hexSelection"
                visible: controller && controller.selectionSummary.length > 0
                text: controller ? controller.selectionSummary : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }

            // The two keys said out loud. A selection that can only leave one way
            // is half a viewer, and nothing else on screen says the other way
            // exists.
            Label {
                visible: controller && controller.selectionSummary.length > 0
                text: "Ctrl+C hex · Ctrl+Shift+C text"
                color: App.colour.textFaint
                font.pixelSize: App.smallTextSize
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "hexPositionLabel"
                text: controller ? controller.positionText : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }
        }

        // An empty file, said rather than shown as a grid with nothing in it.
        Label {
            objectName: "hexEmptyNote"
            Layout.fillWidth: true
            Layout.margins: 12
            visible: controller ? controller.emptyFile === true : false
            text: "This file is empty — there are no bytes to show."
            color: App.colour.textMuted
            font.pixelSize: App.secondaryTextSize
        }

        ListView {
            id: grid
            objectName: "hexRows"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.rows.length > 0
            clip: true
            model: view.rows

            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                required property int index
                required property var modelData

                width: grid.width
                height: view.rowHeight

                readonly property int rowFirstByte:
                    (controller ? controller.windowOffset : 0) + index * view.bytesPerRow
                // The part of the selection that falls in this row, in bytes
                // from the start of it. -1 when none of it does.
                readonly property int selectedFrom: {
                    if (!controller || controller.selectionLength <= 0)
                        return -1
                    const from = Math.max(controller.selectionStart, rowFirstByte)
                    const to = Math.min(controller.selectionStart + controller.selectionLength - 1,
                                        rowFirstByte + view.bytesPerRow - 1)
                    return to >= from ? from - rowFirstByte : -1
                }
                readonly property int selectedCount: {
                    if (selectedFrom < 0)
                        return 0
                    const to = Math.min(controller.selectionStart + controller.selectionLength - 1,
                                        rowFirstByte + view.bytesPerRow - 1)
                    return to - rowFirstByte - selectedFrom + 1
                }

                Row {
                    id: columns
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: view.gutter
                    spacing: view.columnGap

                    Label {
                        text: modelData.offset
                        color: App.colour.textFaint
                        font.family: App.monospaceFont
                        font.pixelSize: App.monospaceSize
                    }

                    Item {
                        width: hexText.implicitWidth
                        height: hexText.implicitHeight

                        Rectangle {
                            visible: selectedFrom >= 0
                            color: Material.accent
                            opacity: 0.28
                            x: view.hexColumn(selectedFrom) * view.cellWidth
                            width: view.hexSpan(selectedFrom, selectedCount) * view.cellWidth
                            height: parent.height
                        }
                        Label {
                            id: hexText
                            text: modelData.hex
                            color: App.colour.textSecondary
                            font.family: App.monospaceFont
                            font.pixelSize: App.monospaceSize
                        }
                    }

                    Item {
                        width: asciiText.implicitWidth
                        height: asciiText.implicitHeight

                        Rectangle {
                            visible: selectedFrom >= 0
                            color: Material.accent
                            opacity: 0.28
                            x: selectedFrom * view.cellWidth
                            width: selectedCount * view.cellWidth
                            height: parent.height
                        }
                        Label {
                            id: asciiText
                            text: modelData.text
                            color: App.colour.textMuted
                            font.family: App.monospaceFont
                            font.pixelSize: App.monospaceSize
                        }
                    }
                }
            }

            // Selection lives above the rows rather than in each delegate, so a
            // drag that leaves the row it started in keeps going: the mouse grab
            // stays here and the row is worked out from the position.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.IBeamCursor
                property int anchorByte: -1

                function byteAt(x, y) {
                    if (!controller)
                        return -1
                    const row = Math.floor((y + grid.contentY) / view.rowHeight)
                    if (row < 0 || row >= view.rows.length)
                        return -1

                    // Which column was hit, and where in it. Every column is
                    // fixed width, so this is arithmetic rather than a hit test.
                    const hexStart = view.gutter + controller.offsetDigits * view.cellWidth + view.columnGap
                    // The hex label is three characters a byte plus the one
                    // space between the groups.
                    const hexChars = view.bytesPerRow * 3 + 1
                    const asciiStart = hexStart + hexChars * view.cellWidth + view.columnGap

                    // `byte` is a reserved word in this dialect, hence `hit`.
                    let hit = 0
                    if (x >= asciiStart)
                        hit = Math.floor((x - asciiStart) / view.cellWidth)
                    else if (x >= hexStart)
                        hit = view.byteAtColumn(Math.floor((x - hexStart) / view.cellWidth))
                    hit = Math.max(0, Math.min(view.bytesPerRow - 1, hit))
                    return controller.windowOffset + row * view.bytesPerRow + hit
                }

                onPressed: function(mouse) {
                    view.forceActiveFocus()
                    anchorByte = byteAt(mouse.x, mouse.y)
                    if (anchorByte >= 0)
                        controller.selectRange(anchorByte, anchorByte)
                    else
                        controller.clearSelection()
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || anchorByte < 0)
                        return
                    const to = byteAt(mouse.x, mouse.y)
                    if (to >= 0)
                        controller.selectRange(anchorByte, to)
                }
            }
        }

        // ---- paging, only for files bigger than one window -----------------

        ToolBar {
            objectName: "hexPagingStrip"
            Layout.fillWidth: true
            visible: controller && controller.paged
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6

                ToolButton {
                    text: "⏮"
                    font.pixelSize: App.textSize
                    enabled: controller && !controller.atStart
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.firstWindow()
                    ToolTip.text: "Start of file  (Ctrl+Home)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
                ToolButton {
                    text: "◀"
                    font.pixelSize: App.textSize
                    enabled: controller && !controller.atStart
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.previousWindow()
                    ToolTip.text: "Previous chunk  (Ctrl+PgUp)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }

                Slider {
                    objectName: "hexPositionSlider"
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    focusPolicy: Qt.NoFocus
                    value: controller && controller.fileSize > 0
                           ? controller.windowOffset / controller.fileSize : 0
                    onMoved: controller.seekToFraction(value)
                }

                ToolButton {
                    text: "▶"
                    font.pixelSize: App.textSize
                    enabled: controller && !controller.atEnd
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.nextWindow()
                    ToolTip.text: "Next chunk  (Ctrl+PgDn)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
                ToolButton {
                    text: "⏭"
                    font.pixelSize: App.textSize
                    enabled: controller && !controller.atEnd
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.lastWindow()
                    ToolTip.text: "End of file  (Ctrl+End)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }

                Label {
                    text: controller ? controller.sizeText : ""
                    color: App.colour.textMuted
                    font.pixelSize: App.smallTextSize
                }
            }
        }
    }
}
