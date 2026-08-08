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

    readonly property bool passwordPossible: App.formatSupportsPassword(formatBox.currentText)

    onAboutToShow: {
        formatBox.currentIndex = 0
        protect.checked = false
        passwordField.text = ""
        subject.text = App.compressionSubject()
        targets.model = App.compressionTargets()
        // Suggested from what is selected, so the common case is one keypress.
        nameField.text = App.suggestedArchiveName(formatBox.currentText)
        nameField.selectAll()
    }
    onOpened: nameField.forceActiveFocus()
    // Only a password that can actually be applied is passed on. The box is
    // disabled for the formats that cannot carry one, and this makes sure a stale
    // one cannot travel with a format that would ignore it.
    onAccepted: App.compressSelection(nameField.text, formatBox.currentText,
                                      (protect.checked && dialog.passwordPossible)
                                          ? passwordField.text : "")

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // What is about to be packed, said before anything happens: the ticked
        // entries, or the row under the cursor, or the folder in view.
        Label {
            id: subject
            objectName: "compressSubject"
            Layout.fillWidth: true
            text: App.compressionSubject()
            font.pixelSize: App.textSize
            elide: Text.ElideMiddle
        }
        // Exactly what goes in, not just how many. A count is a summary; the point of
        // a dialog before an operation is to be able to see that it is aimed at the
        // right things.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.min(140, Math.max(App.listRowHeight, list.contentHeight + 2))
            color: "#151922"
            border.color: "#2a3140"
            border.width: 1
            radius: 3

            ListView {
                id: list
                objectName: "compressTargetList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: targets.model
                ScrollBar.vertical: ScrollBar {}

                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    height: App.listRowHeight
                    spacing: 6

                    Label {
                        Layout.leftMargin: 8
                        text: modelData.isDir ? "📁" : "📄"
                        font.pixelSize: App.secondaryTextSize
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.rightMargin: 8
                        text: modelData.isDir ? modelData.name + "/" : modelData.name
                        elide: Text.ElideMiddle
                        font.pixelSize: App.secondaryTextSize
                    }
                }
            }
        }

        // Holds the model, so onAboutToShow can refill it without the ListView
        // rebinding to something that no longer exists.
        QtObject {
            id: targets
            property var model: []
        }

        Label {
            Layout.fillWidth: true
            text: "A folder goes in with everything inside it. The archive lands next to it."
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
                // The suffix changes; the name does not. Regenerating it from the
                // selection here threw away whatever had been typed, which is how a
                // file came to be saved under the suggested name instead of the
                // chosen one.
                onActivated: nameField.text = App.archiveNameForFormat(nameField.text, currentText)
            }
            Item { Layout.fillWidth: true }
        }

        // A password only where the format can carry one. Offering a box that would
        // be ignored is worse than not offering it: someone would type into it and
        // believe the result was protected.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            CheckBox {
                id: protect
                objectName: "protectWithPassword"
                text: "Password"
                enabled: dialog.passwordPossible
                font.pixelSize: App.secondaryTextSize
            }
            TextField {
                id: passwordField
                objectName: "archivePasswordField"
                Layout.fillWidth: true
                enabled: protect.checked && dialog.passwordPossible
                echoMode: TextInput.Password
                placeholderText: "AES-256, for the contents"
                font.pixelSize: App.textSize
            }
        }

        Label {
            Layout.fillWidth: true
            visible: App.formatTakesOneFileOnly(formatBox.currentText) && targets.model.length !== 1
            text: "A bare .xz holds one file and no folders. Use tar.xz for anything else."
            color: "#e5534b"
            font.pixelSize: App.smallTextSize
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !dialog.passwordPossible
            text: "Only zip can carry a password — a tar has no notion of one, and gzip "
                  + "and xz encrypt nothing."
            color: "#d9a441"
            font.pixelSize: App.smallTextSize
            wrapMode: Text.Wrap
        }
    }
}
