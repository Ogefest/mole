import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A PDF as a column of pages.
//
// Pages are rendered on demand rather than up front: the delegate asks the
// controller for an image when it comes into view, so opening a six-hundred-page
// scan costs the first page and not the six hundred.
Item {
    id: view
    property var controller: null

    readonly property int pageCount: controller ? controller.pageCount : 0

    // Quantised, and that is not a detail: the width goes into the rendered
    // file's name, so binding it to the raw width would render a fresh copy of
    // every visible page for every pixel of a window drag.
    readonly property int renderWidth: Math.max(200, Math.round((pages.width - 48) / 80) * 80)

    ViewerKeys {
        id: viewerKeys
        reserved: [[Qt.Key_PageDown, Qt.ControlModifier], [Qt.Key_PageUp, Qt.ControlModifier],
                   [Qt.Key_Home, Qt.ControlModifier], [Qt.Key_End, Qt.ControlModifier]]
    }

    // Paging by keyboard, the same keys the text viewer uses for the same job.
    Keys.onPressed: function(event) {
        if (!controller || view.pageCount <= 0)
            return
        if (event.key === Qt.Key_PageDown && (event.modifiers & Qt.ControlModifier)) {
            pages.positionViewAtIndex(Math.min(controller.currentPage + 1, view.pageCount - 1),
                                      ListView.Beginning)
            event.accepted = true
        } else if (event.key === Qt.Key_PageUp && (event.modifiers & Qt.ControlModifier)) {
            pages.positionViewAtIndex(Math.max(controller.currentPage - 1, 0), ListView.Beginning)
            event.accepted = true
        } else if (event.key === Qt.Key_Home && (event.modifiers & Qt.ControlModifier)) {
            pages.positionViewAtBeginning()
            event.accepted = true
        } else if (event.key === Qt.Key_End && (event.modifiers & Qt.ControlModifier)) {
            pages.positionViewAtEnd()
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
                // Spinning while a page is being rasterised as well as while the
                // document is being fetched: both are the pane waiting for
                // something, and only one of them used to show.
                running: controller ? controller.loading || controller.renderNote.length > 0 : false
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
                objectName: "pdfTitle"
                visible: controller && controller.errorText.length === 0
                text: controller ? controller.title : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
                elide: Text.ElideMiddle
            }

            Item { Layout.fillWidth: true }

            // A page that takes a second has to say so where the pane says
            // everything else, or it reads as a page that is blank.
            Label {
                objectName: "pdfRenderNote"
                visible: controller && controller.renderNote.length > 0
                text: controller ? controller.renderNote : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }

            Label {
                objectName: "pdfPosition"
                text: controller ? controller.positionText : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }
        }

        ListView {
            id: pages
            objectName: "pdfPages"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 12
            model: view.pageCount
            cacheBuffer: 0

            ScrollBar.vertical: ScrollBar {}

            // What the position strip reports: whichever page is at the top of
            // the viewport, which is the one being read.
            onContentYChanged: if (controller) controller.currentPage = indexAt(width / 2, contentY + 8)

            delegate: Item {
                required property int index

                width: pages.width
                // Reserved from the page's own aspect before anything is
                // rendered, so the list does not jump about as images arrive.
                height: controller
                        ? Math.round(view.renderWidth * controller.pageAspect(index)) + 8
                        : 400

                Rectangle {
                    anchors.centerIn: parent
                    width: page.width
                    height: page.height
                    color: "white" // a page is white, whatever the window is
                    border.color: App.colour.border
                    border.width: 1
                }

                Image {
                    id: page
                    objectName: "pdfPage"
                    anchors.centerIn: parent
                    width: view.renderWidth
                    height: Math.round(view.renderWidth * (controller ? controller.pageAspect(index) : 1.414))
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    // Asked for when this delegate exists, which is when the page
                    // is at or near the viewport -- that is the whole lazy part.
                    //
                    // Nothing here rasterises. The controller answers with nothing
                    // for a page it has not rendered yet and puts it on a task, so
                    // a page of vector art costs the window nothing while it draws
                    // -- and `pagesRendered` is read on purpose: a render landing
                    // is what makes an answer available, and a binding that did
                    // not watch it would never ask again. See MOLE-286.
                    source: controller && controller.pagesRendered >= 0
                            ? controller.pageImage(index, view.renderWidth)
                            : ""
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    text: index + 1
                    color: App.colour.textFaint
                    font.pixelSize: App.smallTextSize
                }
            }

            Keys.onPressed: function(event) { viewerKeys.relay(event) }
        }
    }
}
