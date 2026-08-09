import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asking for the passphrase that opens the credential store.
//
// One component rather than one per place that asks, because the copy is the
// part that matters. The sentence about the passphrase not being tied to this
// computer is the thing people need to hear exactly once, and a second copy of
// it is a second copy to keep true.
//
// Never a modal. The application is perfectly usable without the store open,
// and interrupting a startup to demand a password for a drive nobody has asked
// for yet is the wrong trade.
Rectangle {
    id: band

    /// Focused when a locked drive's row asks for the passphrase, so the row
    /// points at this rather than opening a second way to do the same thing.
    property alias field: passphraseField

    radius: 4
    color: "#2a2418"
    border.color: "#d9a441"
    implicitHeight: content.implicitHeight + 18

    function focusField() {
        passphraseField.forceActiveFocus()
        passphraseField.selectAll()
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 9
        spacing: 6

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#e8c07d"
            font.pixelSize: 11
            text: App.credentialsExist
                  ? "Passwords are encrypted with a passphrase you choose. Enter it to "
                    + "use drives that need one."
                  : "Passwords are encrypted with a passphrase you choose. It is not "
                    + "stored anywhere, and it is not tied to this computer — back up "
                    + "the configuration and the same passphrase opens it on a fresh "
                    + "install."
        }

        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: passphraseField
                objectName: "passphraseField"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: App.credentialsExist ? "Passphrase" : "Choose a passphrase"
                onAccepted: unlockButton.clicked()
            }
            Button {
                id: unlockButton
                objectName: "unlockButton"
                text: App.credentialsExist ? "Unlock" : "Set"
                enabled: passphraseField.text.length > 0
                onClicked: {
                    if (App.unlockCredentials(passphraseField.text)) {
                        passphraseField.text = ""
                        unlockError.text = ""
                    } else {
                        unlockError.text = App.credentialsError()
                    }
                }
            }
        }

        Label {
            id: unlockError
            objectName: "unlockError"
            Layout.fillWidth: true
            visible: text.length > 0
            color: "#e5534b"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
    }
}
