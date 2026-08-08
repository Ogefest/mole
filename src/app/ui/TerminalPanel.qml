import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A shell for the folder you are looking at, along the bottom of the window.
//
// Part of the shell rather than a tab, because that is what it is for: run a
// command *here* and read what it says without leaving the listing.
Rectangle {
    id: panel

    property var terminal: null
    readonly property color panelColor: "#12151b"

    color: panelColor

    // The xterm palette. Terminals refer to colours by index, so the mapping has
    // to live somewhere, and matching what everything else uses means output
    // looks the way its author intended.
    readonly property var palette: [
        "#1b2029", "#e5534b", "#57ab5a", "#d9a441", "#4c9aff", "#c792ea", "#5bc8d6", "#c9d1e0",
        "#5c6472", "#ff7b72", "#7ee787", "#f0c674", "#7cc4ff", "#d2a8ff", "#86d9e8", "#f0f6fc"
    ]

    function colourFor(index, fallback) {
        if (index < 0)
            return fallback
        if (index < palette.length)
            return palette[index]
        // The 6×6×6 cube and the greys above it. Computed rather than tabulated,
        // because 240 hard-coded strings would be 240 chances to mistype one.
        if (index < 232) {
            const n = index - 16
            const r = Math.floor(n / 36), g = Math.floor((n % 36) / 6), b = n % 6
            const level = function(v) { return v === 0 ? 0 : 55 + v * 40 }
            return Qt.rgba(level(r) / 255, level(g) / 255, level(b) / 255, 1)
        }
        const grey = (8 + (index - 232) * 10) / 255
        return Qt.rgba(grey, grey, grey, 1)
    }

    // Measured, so the shell is told a window size that matches what is drawn.
    // Getting this wrong makes everything full-screen wrap in the wrong place.
    TextMetrics {
        id: metrics
        font.family: App.monospaceFont
        font.pixelSize: 12
        text: "M"
    }

    function updateSize() {
        if (!terminal || metrics.advanceWidth <= 0 || metrics.height <= 0)
            return
        const columns = Math.max(20, Math.floor((grid.width - 12) / metrics.advanceWidth))
        const rows = Math.max(4, Math.floor(grid.height / metrics.height))
        terminal.setSize(columns, rows)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            Material.background: "#1b2029"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    text: "Terminal"
                    font.pixelSize: 11
                    font.bold: true
                    color: "#8b93a7"
                }
                Label {
                    Layout.fillWidth: true
                    text: terminal ? (terminal.title.length > 0 ? terminal.title
                                                                : terminal.workingDirectory) : ""
                    color: "#6f7788"
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }

                // Said once, plainly, rather than discovered when an editor
                // draws nonsense.
                Label {
                    id: basicModeTag
                    visible: terminal && !terminal.complete
                    text: "basic mode"
                    color: "#d9a441"
                    font.pixelSize: 10

                    MouseArea {
                        id: basicModeHover
                        anchors.fill: parent
                        hoverEnabled: true
                    }
                    ToolTip.visible: basicModeHover.containsMouse
                    ToolTip.text: "Built without libvterm: line output works, full-screen "
                                  + "programs will not"
                }

                ToolButton {
                    text: "×"
                    implicitWidth: 22
                    implicitHeight: 22
                    focusPolicy: Qt.NoFocus
                    onClicked: terminal.visible = false
                }
            }
        }

        Item {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            onWidthChanged: panel.updateSize()
            onHeightChanged: panel.updateSize()
            Component.onCompleted: panel.updateSize()

            Label {
                anchors.centerIn: parent
                width: parent.width - 40
                visible: terminal && terminal.errorText.length > 0
                text: terminal ? terminal.errorText : ""
                color: "#d9a441"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }

            FocusScope {
                id: keyboard
                anchors.fill: parent
                focus: true

                // Everything goes to the shell, including keys the window would
                // otherwise claim -- a terminal that swallowed Ctrl+C would be
                // useless, and one that let the window take it worse.
                Keys.onPressed: function(event) {
                    if (!terminal)
                        return
                    if (event.key === Qt.Key_QuoteLeft && (event.modifiers & Qt.ControlModifier)) {
                        terminal.visible = false
                        event.accepted = true
                        return
                    }
                    terminal.sendKey(event.key, event.modifiers, event.text)
                    event.accepted = true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: keyboard.forceActiveFocus()
                }

                Column {
                    x: 6
                    y: 2
                    spacing: 0

                    Repeater {
                        // Bound to a property, so the screen redraws as the
                        // shell writes to it. A method call here would be
                        // evaluated once and the panel would stay blank.
                        model: terminal ? terminal.screenRows : []

                        delegate: Row {
                            required property var modelData
                            height: metrics.height
                            spacing: 0

                            Repeater {
                                model: modelData

                                delegate: Rectangle {
                                    required property var modelData
                                    height: metrics.height
                                    width: spanText.implicitWidth
                                    color: modelData.inverse
                                           ? panel.colourFor(modelData.foreground, "#c9d1e0")
                                           : panel.colourFor(modelData.background, "transparent")

                                    Text {
                                        id: spanText
                                        text: modelData.text
                                        font.family: App.monospaceFont
                                        font.pixelSize: 12
                                        font.bold: modelData.bold
                                        color: modelData.inverse
                                               ? panel.colourFor(modelData.background, "#12151b")
                                               : panel.colourFor(modelData.foreground, "#c9d1e0")
                                    }
                                }
                            }
                        }
                    }
                }

                // A block where the shell says the cursor is, so typing has a
                // visible home.
                Rectangle {
                    visible: terminal && terminal.running && keyboard.activeFocus
                    x: 6 + (terminal ? terminal.cursorColumn : 0) * metrics.advanceWidth
                    y: 2 + (terminal ? terminal.cursorRow : 0) * metrics.height
                    width: metrics.advanceWidth
                    height: metrics.height
                    color: Material.accent
                    opacity: 0.55
                }
            }
        }
    }
}
