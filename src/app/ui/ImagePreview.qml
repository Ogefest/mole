import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Fits the image to the frame and offers 1:1. Nothing fancier: a preview is
// for recognising a file, not for editing it.
Item {
    id: view
    property var controller: null
    property bool actualSize: false

    Flickable {
        anchors.fill: parent
        anchors.margins: 8
        contentWidth: Math.max(width, picture.width)
        contentHeight: Math.max(height, picture.height)
        clip: true
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        Image {
            id: picture
            anchors.centerIn: parent
            source: controller ? controller.source : ""
            asynchronous: true
            // Large photographs are downscaled on load rather than held at
            // full resolution just to be shrunk for display.
            //
            // **Quantised, because changing sourceSize makes Image reload.** This
            // was bound to the live pane width, so dragging the sidebar or the
            // details divider issued a decode per pixel of width -- the picture
            // flickering through Image.Loading for the length of the drag, on
            // every intermediate size nobody was looking at. Rounded up to the
            // next 256 px, so a drag across a pane costs a handful of decodes
            // instead of hundreds and the result is never smaller than the pane
            // asks for. See MOLE-398.
            sourceSize.width: view.actualSize
                              ? 0
                              : Math.ceil(Math.max(1, view.width * 2) / 256) * 256
            fillMode: Image.PreserveAspectFit
            width: view.actualSize ? implicitWidth : Math.min(implicitWidth, view.width - 16)
            height: view.actualSize ? implicitHeight : Math.min(implicitHeight, view.height - 40)
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: (controller && controller.loading) || picture.status === Image.Loading
        visible: running
    }

    // A refusal is handed back rather than shown here. The suffixes this viewer
    // claims are the ones Qt says it has plugins for, and a file with one of them
    // can still be truncated or misnamed -- so the decode is where it is found
    // out, and the answer is the list of facts about the file rather than a
    // sentence in an empty frame. The strip says which viewer gave up and why.
    Connections {
        target: picture
        function onStatusChanged() {
            if (picture.status !== Image.Error || !controller || !controller.reportDecodeFailure)
                return

            // Two different answers arrive through this one status, and telling
            // them apart is the whole of MOLE-288. A decode that failed at full
            // size failed because of what was asked for: the fitted picture was
            // on screen a moment ago, so the view goes back to it and the
            // controller withdraws 1:1 rather than handing the file down the
            // ladder to a page of metadata.
            if (view.actualSize) {
                view.actualSize = false
                controller.reportDecodeFailure(true)
                return
            }
            controller.reportDecodeFailure()
        }
    }

    // Said here as well, the way every other viewer says it. In this application
    // the pane is replaced before anybody reads it -- the tab has somewhere to
    // step down to -- and a viewer that reported a refusal nowhere would be one
    // that goes silent if it ever did not.
    Label {
        anchors.centerIn: parent
        visible: controller && controller.errorText.length > 0
        wrapMode: Text.Wrap
        color: App.colour.bad
        text: controller ? controller.errorText : ""
    }

    RowLayout {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 8
        spacing: 8

        Label {
            visible: picture.status === Image.Ready
            text: picture.implicitWidth + " × " + picture.implicitHeight
            color: App.colour.textMuted
            font.pixelSize: App.smallTextSize
        }
        Button {
            text: view.actualSize ? "Fit" : "1:1"
            // Going back to a fitted view always works. Leaving it is what the
            // controller has an opinion about.
            enabled: view.actualSize || !controller || controller.actualSizeAvailable
            flat: true
            font.pixelSize: App.smallTextSize
            focusPolicy: Qt.NoFocus
            onClicked: view.actualSize = !view.actualSize
        }
        // Why it is greyed. A disabled button with nothing beside it reads as
        // the application being broken rather than as the picture being larger
        // than this build will decode at once.
        Label {
            visible: !view.actualSize && controller && !controller.actualSizeAvailable
            text: controller ? controller.actualSizeReason : ""
            color: App.colour.textMuted
            font.pixelSize: App.smallTextSize
        }
    }
}
