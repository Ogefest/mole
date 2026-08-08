import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The fallback. When nothing can render a file, say what is known about it
// rather than showing an empty frame.
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
                font.pixelSize: 12
                text: "No installed viewer handles this file type. A plugin can add one."
            }

            Repeater {
                model: controller ? controller.facts : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        Layout.preferredWidth: 110
                        text: modelData.label
                        color: "#6f7788"
                        font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.value
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
