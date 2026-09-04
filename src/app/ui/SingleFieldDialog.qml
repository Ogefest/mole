import QtQuick
import QtQuick.Controls

// A dialog that asks for one thing: a folder name, a new name, a passphrase.
//
// FilePane's mkdir and rename dialogs were the same dialog twice with two words
// changed, which is how one of them came to keep `selectByMouse` and the other
// not. See MoleDialog.qml and MOLE-398.
MoleDialog {
    id: asker

    /// What the field says before anything is typed.
    property alias placeholder: field.placeholderText
    /// What it holds. Set it in `onOpened` -- the dialog does not guess.
    property alias text: field.text
    /// Whether opening selects what is already there, which is what a rename
    /// wants and a new name does not.
    property bool selectOnOpen: false

    footer: ConfirmButtons { acceptText: asker.acceptText }
    /// The word on the accepting button.
    property string acceptText: "OK"

    onOpened: {
        field.forceActiveFocus()
        if (asker.selectOnOpen)
            field.selectAll()
    }

    TextField {
        id: field
        anchors.fill: parent
        selectByMouse: true
        onAccepted: asker.accept()
    }
}
