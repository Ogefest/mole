import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Text and source code, coloured; Markdown rendered.
//
// The controller only ever holds the window being shown, so this view is the
// same whether the file is a note or a 100 GB log -- the paging strip is the
// only difference, and it appears only when there is more than one window.
Item {
    id: view
    property var controller: null

    readonly property bool showsMarkdown: controller ? controller.markdown : false

    // Colouring has to attach to the real QTextDocument behind the TextArea;
    // there is no way to do it from QML alone.
    function attachHighlighter() {
        if (controller && controller.attachHighlighter && !view.showsMarkdown)
            controller.attachHighlighter(area.textDocument)
    }

    onControllerChanged: Qt.callLater(attachHighlighter)
    Component.onCompleted: Qt.callLater(attachHighlighter)

    ViewerKeys {
        id: viewerKeys
        // This view pages with these, so they stay here rather than being
        // handed back to the window.
        reserved: [[Qt.Key_PageDown, Qt.ControlModifier], [Qt.Key_PageUp, Qt.ControlModifier],
                   [Qt.Key_Home, Qt.ControlModifier], [Qt.Key_End, Qt.ControlModifier]]
    }

    // Paging by keyboard, because that is how anyone reads a long file.
    Keys.onPressed: function(event) {
        if (!controller || !controller.paged)
            return
        if (event.key === Qt.Key_PageDown && (event.modifiers & Qt.ControlModifier)) {
            controller.nextWindow()
            event.accepted = true
        } else if (event.key === Qt.Key_PageUp && (event.modifiers & Qt.ControlModifier)) {
            controller.previousWindow()
            event.accepted = true
        } else if (event.key === Qt.Key_Home && (event.modifiers & Qt.ControlModifier)) {
            controller.firstWindow()
            event.accepted = true
        } else if (event.key === Qt.Key_End && (event.modifiers & Qt.ControlModifier)) {
            controller.lastWindow()
            event.accepted = true
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
                Layout.fillWidth: true
                visible: controller && controller.errorText.length > 0
                text: controller ? controller.errorText : ""
                color: Material.color(Material.Red)
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }

            Label {
                visible: controller && controller.languageName.length > 0
                text: controller ? controller.languageName : ""
                color: "#8b93a7"
                font.pixelSize: 11
            }

            Label {
                visible: view.showsMarkdown
                text: "Markdown"
                color: "#8b93a7"
                font.pixelSize: 11
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "positionLabel"
                text: controller ? controller.positionText : ""
                color: "#8b93a7"
                font.pixelSize: 11
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: area
                objectName: "previewText"
                readOnly: true
                selectByMouse: true
                wrapMode: view.showsMarkdown ? TextArea.Wrap : TextArea.NoWrap
                // One family for every code and data view, chosen once by the
                // application rather than guessed per view.
                font.family: view.showsMarkdown ? Qt.application.font.family : App.monospaceFont
                font.pixelSize: view.showsMarkdown ? 14 : 12
                // Markdown is rendered; everything else stays plain, because
                // letting the TextArea parse markup would swallow the very
                // tags an XML preview exists to show.
                textFormat: view.showsMarkdown ? TextEdit.MarkdownText : TextEdit.PlainText
                // Not re-attached on every text change: attaching rehighlights,
                // which changes the text, which would call this again. The
                // document object outlives each file, and the controller sets
                // the language itself, so once is enough.
                text: controller ? controller.text : ""

                // A read-only editor still claims Qt's editing shortcuts, which
                // is why Ctrl+W stopped closing the tab once the body had the
                // keyboard. See ViewerKeys.qml.
                Keys.onPressed: function(event) { viewerKeys.relay(event) }
            }
        }

        // ---- paging, only for files bigger than one window -----------------

        ToolBar {
            objectName: "pagingStrip"
            Layout.fillWidth: true
            visible: controller && controller.paged
            Material.background: "#1b2029"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6

                ToolButton {
                    text: "⏮"
                    enabled: controller && !controller.atStart
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.firstWindow()
                    ToolTip.text: "Start of file  (Ctrl+Home)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
                ToolButton {
                    text: "◀"
                    enabled: controller && !controller.atStart
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.previousWindow()
                    ToolTip.text: "Previous chunk  (Ctrl+PgUp)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }

                // Dragging jumps straight to a point in the file: with no index
                // there is no line number to ask for, but a position there is.
                Slider {
                    objectName: "positionSlider"
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
                    enabled: controller && !controller.atEnd
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.nextWindow()
                    ToolTip.text: "Next chunk  (Ctrl+PgDn)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
                ToolButton {
                    text: "⏭"
                    enabled: controller && !controller.atEnd
                    focusPolicy: Qt.NoFocus
                    onClicked: controller.lastWindow()
                    ToolTip.text: "End of file  (Ctrl+End)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }

                Label {
                    text: controller ? controller.sizeText : ""
                    color: "#8b93a7"
                    font.pixelSize: 11
                }
            }
        }
    }
}
