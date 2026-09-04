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
        applyDocumentStyle()
    }

    // What the window knows that a document has to follow: which way up the theme
    // is, and the three places the window's own paint shows through a rendered
    // page -- the slab behind a code fence, a quotation, a table rule. Sent from
    // here because the palette belongs to the view, and sent again when the theme
    // changes, or a file already open would keep the colours it was opened under.
    function applyDocumentStyle() {
        if (controller && controller.setDocumentStyle)
            controller.setDocumentStyle(App.colour.light, App.colour.hover,
                                        App.colour.textMuted, App.colour.border)
    }

    Connections {
        target: App.colour
        function onChanged() { view.applyDocumentStyle() }
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

    // ---- finding a word in what is on screen ---------------------------
    //
    // Ctrl+F is not available and is not taken: it opens a search of the folder
    // in a new tab, which is a different and more valuable thing. So the way in
    // is the gesture this application already uses for exactly this purpose in
    // whichever pane has focus -- start typing, which the keyboard help calls
    // "just start typing — filter this folder". Here it means find in this file.
    // `/` does the same explicitly, for anyone who reads logs in a pager and
    // reaches for it without thinking. See MOLE-308.
    property bool finding: false

    function beginFind(initial) {
        view.finding = true
        findField.text = initial !== undefined ? initial : ""
        findField.forceActiveFocus()
        findField.cursorPosition = findField.text.length
    }

    function endFind() {
        view.finding = false
        findField.text = ""
        if (controller && controller.clearFind)
            controller.clearFind()
        area.forceActiveFocus()
    }

    // Paging by keyboard, because that is how anyone reads a long file.
    Keys.onPressed: function(event) {
        if (controller) {
            if (event.key === Qt.Key_Escape && view.finding) {
                view.endFind()
                event.accepted = true
                return
            }
            // Guarded on the modifiers, or Ctrl+D would type a "d" into the bar
            // instead of adding a bookmark -- the same guard FilePane makes.
            if (!view.finding && event.text.length > 0
                    && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                var ch = event.text
                // Not a plain space: in a preview tab Space steps to the next
                // file, and this claimed it first whenever a text viewer was
                // loaded -- so one key did two things depending on what was on
                // screen, and opened the find bar holding a space. A leading
                // space is not a search term anybody typed on purpose. See
                // PreviewView.qml and MOLE-398.
                if (ch >= " " && ch !== " " && ch !== "\u007f") {
                    // `/` opens it empty; anything else opens it holding what was
                    // typed, so the first keystroke is not lost.
                    view.beginFind(ch === "/" ? "" : ch)
                    event.accepted = true
                    return
                }
            }
        }
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
                color: App.colour.bad
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

            // Beside it and in the same colour, because it is the same kind of
            // fact: what is on the screen is not what the file would normally be
            // shown as, and the reader has to be told rather than left to wonder
            // why the markup is showing. The strip's own Show control is how they
            // ask for the page anyway.
            Label {
                objectName: "markdownDeclinedNote"
                visible: controller ? controller.markdownDeclined === true : false
                text: controller ? controller.markdownDeclinedNote : ""
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

        // ---- the text, with a gutter of line numbers beside it -------------
        //
        // Numbers where there is no paging, and no gutter at all where there is:
        // a number here always means which line of the file this is. See
        // MOLE-309 and the note on TextPreviewController::numbered.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Item {
                id: numbers
                objectName: "lineNumbers"
                visible: controller ? controller.numbered === true : false
                Layout.fillHeight: true
                // Sized from the widest number there will be, so the text does
                // not shift a column as a reader scrolls into four figures.
                Layout.preferredWidth: visible
                    ? numberMetrics.width * Math.max(2, controller.lineNumberDigits) + 16
                    : 0
                clip: true

                TextMetrics {
                    id: numberMetrics
                    font.family: App.monospaceFont
                    font.pixelSize: view.bodyPixelSize
                    text: "0"
                }

                // Taken from the text's own layout rather than from the font's
                // metrics: one row per block with wrapping off, so this is exact,
                // and a leading the metrics do not report would drift a number
                // away from its line a few hundred rows down.
                readonly property real rowHeight: (area.contentHeight > 0 && numberList.count > 0)
                                                  ? area.contentHeight / numberList.count
                                                  : numberMetrics.height

                Rectangle {
                    anchors.fill: parent
                    color: App.colour.panel
                }

                // A list rather than a Repeater: ten thousand lines is an ordinary
                // source file, and a delegate per row that is recycled costs what
                // is on screen instead of what is in the file.
                ListView {
                    id: numberList
                    anchors.fill: parent
                    anchors.topMargin: area.topPadding
                    interactive: false
                    // Follows the text rather than scrolling itself, which is what
                    // keeps a number beside its line.
                    contentY: scroll.contentItem ? scroll.contentItem.contentY : 0
                    model: controller ? controller.lineNumbers : []
                    delegate: Text {
                        width: numberList.width - 8
                        height: numbers.rowHeight
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        font.family: App.monospaceFont
                        font.pixelSize: view.bodyPixelSize
                        color: App.colour.textMuted
                        // 0 is a row a fold made rather than a line of the file,
                        // and it is left blank: a folded run is one line, however
                        // many rows it is shown as.
                        text: modelData > 0 ? modelData : ""
                    }
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

                // The current match is scrolled to by moving the cursor, which is
                // the one thing a TextArea does for free -- and it leaves the
                // marks alone, because they are the document's formats rather
                // than a selection.
                Connections {
                    target: controller
                    enabled: controller !== null
                    function onFindChanged() {
                        if (!controller || controller.findPosition < 0)
                            return
                        area.cursorPosition = controller.findPosition
                    }
                }
            }
        }
        }

        // ---- the find bar, only while somebody is looking ------------------

        ToolBar {
            objectName: "findBar"
            Layout.fillWidth: true
            visible: view.finding
            Material.background: App.colour.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 8

                Label {
                    text: "Find"
                    color: App.colour.textMuted
                }

                TextField {
                    id: findField
                    objectName: "findField"
                    Layout.fillWidth: true
                    Layout.maximumWidth: 320
                    placeholderText: "a word in this window"
                    // Every keystroke, like the filter box in the grid next door:
                    // a reader watching the count decides whether to keep typing.
                    onTextChanged: if (controller && controller.find) controller.find(text)
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape) {
                            view.endFind()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            if (event.modifiers & Qt.ShiftModifier)
                                controller.findPrevious()
                            else
                                controller.findNext()
                            event.accepted = true
                        }
                    }
                }

                // Which of how many, because a count is what tells a reader
                // whether to keep looking or to page on -- and where the file is
                // being shown a window at a time it says so, since "no matches"
                // about a window reads as "not in this file".
                Label {
                    objectName: "findCount"
                    text: controller ? controller.findSummary : ""
                    color: App.colour.textMuted
                }

                ToolButton {
                    objectName: "findPrevious"
                    text: "\u2191"
                    enabled: controller && controller.findCount > 0
                    onClicked: controller.findPrevious()
                }

                ToolButton {
                    objectName: "findNext"
                    text: "\u2193"
                    enabled: controller && controller.findCount > 0
                    onClicked: controller.findNext()
                }

                ToolButton {
                    objectName: "findClose"
                    text: "\u2715"
                    onClicked: view.endFind()
                }
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
