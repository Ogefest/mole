import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asking for the passphrase that opens the credential store.
//
// One component, and the only place the copy lives. The sentence about the
// passphrase not being tied to this computer is the thing people need to hear
// exactly once, and a second copy of it is a second copy to keep true — which is
// why the drives dialog opens this rather than growing its own panel.
//
// A modal, and asked for at the moment somebody opens a drive that needs it.
// This reverses half of an earlier decision, which was that the store must never
// be asked for in a modal; the reason behind that stands and is why the other
// half was kept. See docs/adr/0031-a-locked-drive-is-connected-when-it-is-opened.md.
Dialog {
    id: dialog
    objectName: "unlockDialog"

    title: App.credentialsExist ? "Unlock the credential store" : "Choose a passphrase"
    modal: true
    // Without this the popup never becomes a focus scope, so nothing inside it
    // can hold the keyboard and forceActiveFocus() quietly does nothing.
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, parent ? parent.width - 80 : 460)

    // Not `Unlock` and not red: opening a store destroys nothing. The button
    // acts without closing, because a passphrase can be refused and the dialog
    // is the only place that can say so.
    footer: ConfirmButtons {
        acceptText: App.credentialsExist ? "Unlock" : "Set"
        acceptEnabled: passphraseField.text.length > 0
        actWithoutClosing: true
        keyboardOn: "none"
    }

    onOpened: {
        unlockError.text = ""
        passphraseField.text = ""
        passphraseField.forceActiveFocus()
    }

    onApplied: {
        if (App.unlockCredentials(passphraseField.text)) {
            // Whatever was waiting on the store — including the drive somebody
            // opened to get here — is the controller's business now.
            passphraseField.text = ""
            unlockError.text = ""
            close()
        } else {
            unlockError.text = App.credentialsError()
            passphraseField.selectAll()
        }
    }

    // Somebody who backs out is not left with a drive half-opened: nothing was
    // connected, and the controller drops whatever navigation was waiting.
    onRejected: App.abandonPendingNavigation()

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#c8cedb"
            font.pixelSize: App.secondaryTextSize
            text: App.credentialsExist
                  ? "Passwords are encrypted with a passphrase you choose. Enter it to "
                    + "use drives that need one."
                  : "Passwords are encrypted with a passphrase you choose. It is not "
                    + "stored anywhere, and it is not tied to this computer — back up "
                    + "the configuration and the same passphrase opens it on a fresh "
                    + "install."
        }

        TextField {
            id: passphraseField
            objectName: "passphraseField"
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: App.credentialsExist ? "Passphrase" : "Choose a passphrase"
            // The keyboard is in here, so Return has to answer from here.
            onAccepted: if (text.length > 0) dialog.applied()
        }

        Label {
            id: unlockError
            objectName: "unlockError"
            Layout.fillWidth: true
            visible: text.length > 0
            color: "#e5534b"
            font.pixelSize: App.secondaryTextSize
            wrapMode: Text.WordWrap
        }
    }
}
