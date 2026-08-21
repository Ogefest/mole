import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The fallback. When nothing can render a file -- an empty one, one that could
// not be read, or one whose format nothing here understands -- say what is known
// about it rather than showing an empty frame.
//
// The facts themselves are not here: they are the details panel above, which
// every viewer has and which this one opens by default. What is left is the name,
// the reason there is nothing to show, and the bytes for whoever came for those.
// See docs/adr/0034-what-a-file-says-about-itself.md.
Item {
    id: view
    property var controller: null
    /// The preview tab, which owns the facts -- the viewer's own controller does
    /// not. Set by PreviewView alongside `controller`.
    property var tab: null

    // This viewer shows the facts whether or not the drawer is open, so it asks
    // for them itself. Nothing is read for a drawer nobody opened; something is
    // read when something is about to show it.
    onTabChanged: if (tab) tab.requestDetails()
    Component.onCompleted: if (tab) tab.requestDetails()
    /// Read by PreviewView: this viewer renders the facts itself, so the drawer
    /// stays out of its way rather than showing them twice.
    readonly property bool showsDetailsItself: !showsBytes

    readonly property bool showsBytes: controller ? controller.showingBytes === true : false

    // The hex window, when Bytes was chosen on the strip. Loaded then and not
    // before, so the choice nobody made costs nothing.
    Loader {
        anchors.fill: parent
        active: view.showsBytes
        source: active ? "HexPreview.qml" : ""
        onLoaded: if (item) item.controller = controller ? controller.bytes : null
    }

    Connections {
        target: controller
        function onFactsChanged() { /* rebinds the loader through showsBytes */ }
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        clip: true
        visible: !view.showsBytes

        ColumnLayout {
            width: parent.width
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: controller ? controller.headline : ""
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideMiddle
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: App.colour.textMuted
                font.pixelSize: App.secondaryTextSize
                text: "No installed viewer handles this file type. A plugin can add one."
            }

            // The facts, in the body rather than in the drawer, because here
            // they are the content. The same component either way.
            DetailsList {
                objectName: "fileInfoDetails"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(120, implicitHeight)
                Layout.minimumHeight: 120
                facts: view.tab ? view.tab.details : []
                busy: view.tab ? view.tab.detailsLoading : false
                onCopyAll: function() { if (view.tab) view.tab.copyDetails() }
            }
        }
    }
}
