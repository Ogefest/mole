import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The two things worth asking before packing: what to call it, and what kind it is.
//
// Everything else -- what is being packed, where it lands -- is already known from
// what is selected, and asking about it would be a form rather than a question.
Dialog {
    id: dialog
    objectName: "compressDialog"

    title: "Compress"
    modal: true
    anchors.centerIn: parent
    standardButtons: Dialog.Ok | Dialog.Cancel
    width: Math.min(460, parent ? parent.width - 80 : 460)

    onAboutToShow: {
        formatBox.currentIndex = 0
        // Suggested from what is selected, so the common case is one keypress.
        nameField.text = App.suggestedArchiveName(formatBox.currentText)
        nameField.selectAll()
    }
    onOpened: nameField.forceActiveFocus()
    onAccepted: App.compressSelection(nameField.text, formatBox.currentText)

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: "It lands next to what is being packed."
            color: "#8b93a7"
            font.pixelSize: App.smallTextSize
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Name"
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
            }
            TextField {
                id: nameField
                objectName: "archiveNameField"
                Layout.fillWidth: true
                font.pixelSize: App.textSize
                selectByMouse: true
                onAccepted: dialog.accept()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Kind"
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
            }
            ComboBox {
                id: formatBox
                objectName: "archiveFormatBox"
                implicitContentWidthPolicy: ComboBox.WidestText
                font.pixelSize: App.secondaryTextSize
                model: App.compressionFormats()
                // Changing the kind renames the suffix rather than leaving a .zip
                // called .tar.gz, which is the sort of thing nobody notices until
                // something refuses to open it.
                onActivated: nameField.text = App.suggestedArchiveName(currentText)
            }
            Item { Layout.fillWidth: true }
        }
    }
}
