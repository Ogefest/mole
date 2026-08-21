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
    // A page rather than its source, when that is what was chosen for this type.
    readonly property bool showsPage: controller ? controller.renderedHtml === true : false

    // Prose and code come from the same scale as everywhere else, and at the same
    // step: a log is the preview left open longest, so it is not the place to save
    // a pixel. The two entries stay separate because the family differs.
    readonly property int bodyPixelSize: (showsMarkdown || showsPage) ? App.textSize : App.monospaceSize

    // A page of prose needs a measure. Text that runs the full width of a wide
    // window is tiring to read -- the eye loses the line coming back -- so the
    // gutters take the surplus and the page stays centred at a readable width.
    readonly property int readingWidth: 760
    readonly property int gutter: (showsMarkdown || showsPage)
        ? Math.max(28, Math.round((scroll.availableWidth - readingWidth) / 2)) : 0

    // Colouring and Markdown typography both have to attach to the real
    // QTextDocument behind the TextArea; there is no way to do either from QML
    // alone. The controller decides which of the two the current file gets.
    function attachDocument() {
        if (controller && controller.attachDocument)
            controller.attachDocument(area.textDocument, view.bodyPixelSize, App.monospaceFont)
    }

    onControllerChanged: Qt.callLater(attachDocument)
    Component.onCompleted: Qt.callLater(attachDocument)

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
                font.pixelSize: App.secondaryTextSize
            }

            Label {
                visible: controller && controller.languageName.length > 0
                text: controller ? controller.languageName : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }

            Label {
                visible: view.showsMarkdown || view.showsPage
                text: view.showsMarkdown ? "Markdown" : "Rendered page"
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }

            // Said out loud, and in the colour of a caveat rather than of a
            // caption: the line breaks in front of the reader are Mole's, not the
            // file's, and somebody comparing this against the original has to
            // know that. "folded" is the word for it because nothing was lost.
            Label {
                objectName: "foldedNote"
                visible: controller ? controller.longLinesFolded === true : false
                text: "long lines folded, colouring off"
                color: App.colour.warn
                font.pixelSize: App.smallTextSize
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "positionLabel"
                text: controller ? controller.positionText : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: area
                objectName: "previewText"
                readOnly: true
                selectByMouse: true
                wrapMode: (view.showsMarkdown || view.showsPage) ? TextArea.Wrap : TextArea.NoWrap
                // One family for every code and data view, chosen once by the
                // application rather than guessed per view.
                font.family: (view.showsMarkdown || view.showsPage)
                             ? Qt.application.font.family : App.monospaceFont
                font.pixelSize: view.bodyPixelSize
                // A rendered page gets margins; a log or a source file does not,
                // because there every column position is information.
                leftPadding: (view.showsMarkdown || view.showsPage) ? view.gutter : padding
                rightPadding: (view.showsMarkdown || view.showsPage) ? view.gutter : padding
                topPadding: (view.showsMarkdown || view.showsPage) ? 24 : padding
                bottomPadding: (view.showsMarkdown || view.showsPage) ? 40 : padding
                // Markdown is rendered, HTML is rendered when asked for, and
                // everything else stays plain -- letting the TextArea parse markup
                // would swallow the very tags an XML preview exists to show.
                textFormat: view.showsMarkdown
                            ? TextEdit.MarkdownText
                            : (view.showsPage ? TextEdit.RichText : TextEdit.PlainText)
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
