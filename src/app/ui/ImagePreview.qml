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
            sourceSize.width: view.actualSize ? 0 : view.width * 2
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

    Label {
        anchors.centerIn: parent
        visible: picture.status === Image.Error
        wrapMode: Text.Wrap
        color: App.colour.bad
        text: "This image could not be decoded by the installed Qt image plugins."
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
            flat: true
            font.pixelSize: App.smallTextSize
            focusPolicy: Qt.NoFocus
            onClicked: view.actualSize = !view.actualSize
        }
    }

    Label {
        anchors.centerIn: parent
        visible: controller && controller.errorText.length > 0
        text: controller ? controller.errorText : ""
        color: App.colour.bad
        wrapMode: Text.Wrap
    }
}
