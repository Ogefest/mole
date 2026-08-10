import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tab body for indexed search: answers come from a previous scan, so they are
// instant and may be stale. The two search tabs stay separate on purpose.
Item {
    id: view

    property var controller: null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                Layout.fillWidth: true
                placeholderText: "Name contains..."
                text: controller ? controller.queryText : ""
                selectByMouse: true
                font.pixelSize: 13
                onTextChanged: if (controller) controller.queryText = text
                onAccepted: if (controller) controller.search()
            }

            ComboBox {
                Layout.preferredWidth: 240
                model: controller ? controller.volumeLabels : []
                currentIndex: controller ? controller.volumeIndex : 0
                font.pixelSize: 12
                onActivated: if (controller) controller.volumeIndex = currentIndex
            }

            Button {
                text: "Search"
                highlighted: true
                onClicked: if (controller) controller.search()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: controller ? controller.statusText : ""
                color: "#8b93a7"
                elide: Text.ElideRight
                font.pixelSize: 12
            }

            BusyIndicator {
                running: controller ? controller.running : false
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }

            Button {
                text: "Scan a folder..."
                flat: true
                font.pixelSize: 12
                onClicked: scanDialog.open()
            }
        }

        SearchResultList {
            // No set from here yet: the index tab has nowhere to put one.
            canBuildSet: false
            Layout.fillWidth: true
            Layout.fillHeight: true
            resultsModel: controller ? controller.results : null
        }
    }

    Dialog {
        id: scanDialog
        objectName: "scanDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        focus: true
        title: "Index a folder"
        modal: true
        anchors.centerIn: parent
        width: 520

        footer: ConfirmButtons {
            acceptText: "Index"
            // Nothing to index without a path, and a button that acts on nothing
            // is worse than one that says it cannot.
            acceptEnabled: scanPath.text.trim().length > 0
            // Typed into first, so the field wins over the button.
            keyboardOn: "none"
        }

        onOpened: scanPath.forceActiveFocus()

        onAccepted: {
            if (controller && scanPath.text.length > 0) {
                var uri = scanPath.text.trim()
                if (uri.indexOf("://") < 0)
                    uri = "file://" + uri
                controller.scanDirectory(uri, scanLabel.text)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Walks the tree once in the background and records it, so searching it later never touches the disk."
                color: "#8b93a7"
                font.pixelSize: 12
            }

            TextField {
                id: scanPath
                objectName: "scanPath"
                Layout.fillWidth: true
                placeholderText: "/home/you/big-archive"
                selectByMouse: true
                // The keyboard starts here, so Return has to answer from here.
                onAccepted: if (text.trim().length > 0) scanDialog.accept()
            }

            TextField {
                id: scanLabel
                Layout.fillWidth: true
                placeholderText: "Label (optional)"
                selectByMouse: true
                onAccepted: if (scanPath.text.trim().length > 0) scanDialog.accept()
            }
        }
    }
}
