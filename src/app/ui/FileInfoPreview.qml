import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The fallback. When nothing can render a file -- an empty one, or one that
// could not be read -- say what is known about it rather than showing an empty
// frame.
//
// The facts themselves are not here any more: they are the details panel above,
// which every viewer has and which this one opens by default. What is left is
// the name and the reason there is nothing to show.
// See docs/adr/0034-what-a-file-says-about-itself.md.
Item {
    property var controller: null

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        clip: true

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
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
                text: "No installed viewer handles this file type. A plugin can add one."
            }

        }
    }
}
