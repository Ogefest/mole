import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
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
MoleDialog {
    id: dialog
    objectName: "unlockDialog"

    title: App.credentialsExist ? "Unlock the credential store" : "Choose a passphrase"
    // Without this the popup never becomes a focus scope, so nothing inside it
    // can hold the keyboard and forceActiveFocus() quietly does nothing.
    preferredWidth: 460

    // Not `Unlock` and not red: opening a store destroys nothing. The button
    // acts without closing, because a passphrase can be refused and the dialog
    // is the only place that can say so.
    footer: ConfirmButtons {
        acceptText: App.credentialsExist ? "Unlock" : "Set"
        // Nothing to press while the key is being derived: it takes a
        // noticeable fraction of a second by design, and pressing again would
        // start a second one.
        acceptEnabled: passphraseField.text.length > 0 && !App.credentialsBusy
        actWithoutClosing: true
        keyboardOn: "none"
    }

    onOpened: {
        unlockError.text = ""
        passphraseField.text = ""
        passphraseField.forceActiveFocus()
    }

    // Started, not done. The derivation runs on a task so the window stays live,
    // and the answer comes back on credentialsAttempted. See ADR-0090.
    onApplied: App.unlockCredentials(passphraseField.text)

    Connections {
        target: App
        function onCredentialsAttempted(ok) {
            if (!dialog.visible)
                return
            if (ok) {
                // Whatever was waiting on the store — including the drive
                // somebody opened to get here — is the controller's business now.
                passphraseField.text = ""
                unlockError.text = ""
                dialog.close()
            } else {
                unlockError.text = App.credentialsError()
                passphraseField.selectAll()
                passphraseField.forceActiveFocus()
            }
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
            color: App.colour.textSecondary
            font.pixelSize: App.secondaryTextSize
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
            spacing: 8

            TextField {
                id: passphraseField
                objectName: "passphraseField"
                Layout.fillWidth: true
                enabled: !App.credentialsBusy
                echoMode: TextInput.Password
                placeholderText: App.credentialsExist ? "Passphrase" : "Choose a passphrase"
                // The keyboard is in here, so Return has to answer from here.
                onAccepted: if (text.length > 0 && !App.credentialsBusy) dialog.applied()
            }

            // Said rather than implied. A window that has gone still for half a
            // second with nothing moving reads as a window that has stopped.
            BusyIndicator {
                objectName: "unlockBusy"
                running: App.credentialsBusy
                visible: running
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        Label {
            id: unlockError
            objectName: "unlockError"
            Layout.fillWidth: true
            visible: text.length > 0
            color: App.colour.bad
            font.pixelSize: App.secondaryTextSize
            wrapMode: Text.WordWrap
        }
    }
}
