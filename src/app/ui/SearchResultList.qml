import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared result list for every search flavour. Rows carry a full uri, so
// "reveal in browser" works whether the hit came from disk or from the index.
Rectangle {
    id: results

    property var resultsModel: null

    color: "#151922"
    border.color: "#2a3140"
    border.width: 1

    ListView {
        anchors.fill: parent
        anchors.margins: 1
        clip: true
        model: results.resultsModel
        reuseItems: true
        ScrollBar.vertical: ScrollBar {}

        delegate: ItemDelegate {
            required property int index
            required property string name
            required property string uri
            required property string parentUri
            required property bool isDir
            required property string sizeText
            required property string iconText

            width: ListView.view.width
            height: 34
            onDoubleClicked: App.goTo(isDir ? uri : parentUri)

            background: Rectangle {
                color: hovered ? "#232a36" : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8

                Label { text: iconText; font.pixelSize: 14 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: name
                        elide: Text.ElideMiddle
                        font.pixelSize: 13
                    }
                    Label {
                        Layout.fillWidth: true
                        text: parentUri
                        elide: Text.ElideLeft
                        color: "#6f7788"
                        font.pixelSize: 10
                    }
                }

                Label {
                    Layout.preferredWidth: 80
                    horizontalAlignment: Text.AlignRight
                    text: sizeText
                    color: "#8b93a7"
                    font.pixelSize: 12
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !results.resultsModel || results.resultsModel.count === 0
        text: "No results yet"
        color: "#6f7788"
    }
}
