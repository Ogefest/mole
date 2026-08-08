import QtQuick
import QtQuick.Controls

// The two ways out of a dialog, told apart at a glance.
//
// The standard buttons are two flat labels of the same weight and colour, so "Yes"
// and "No" -- or worse, "Ok" and "Cancel" over something irreversible -- read as one
// undifferentiated pair, and neither of them holds the keyboard. This says which one
// acts, what it will do, and where the keyboard is.
//
// Three rules, in docs/adr/0010-telling-the-two-buttons-apart.md:
//   - the button that acts is filled; the one that backs out stays outlined;
//   - it is filled red when what it does cannot be undone;
//   - a destructive dialog opens with the keyboard on the safe way out, so a stray
//     Return or Space closes it rather than committing to it.
//
// The backgrounds are drawn here rather than left to the Material style. Asking that
// style for a highlighted button gives an item that reports itself visible, sized and
// filled red -- and paints nothing, which a screenshot with no red pixel anywhere in
// the footer is what finally showed.
DialogButtonBox {
    id: box

    /// What the acting button does, as a verb: "Delete", "Compress", "Rename".
    /// "Ok" says nothing about which of the two is which.
    property string acceptText: "Ok"
    property string rejectText: "Cancel"
    /// Irreversible: the acting button turns red and the keyboard starts on the
    /// other one.
    property bool destructive: false
    property bool acceptEnabled: true
    /// For dialogs that want the keyboard somewhere else -- a name field being
    /// typed into, say.
    property bool takeFocus: destructive

    readonly property color actingColour: destructive ? "#c0392b" : "#2d6cdf"
    readonly property color focusRing: "#9db4ff"

    // Three scopes deep, and every one of them has to be holding the keyboard for the
    // button at the bottom to have it: the popup, this box, and the ListView the box
    // lays its buttons out with. Miss the middle one and the focus is set, reported as
    // set, and never active.
    focus: takeFocus

    Binding {
        target: box.contentItem
        property: "focus"
        value: box.takeFocus
    }

    Button {
        id: rejectButton
        objectName: "dialogReject"
        text: box.rejectText
        flat: true
        focus: box.takeFocus
        font.pixelSize: App.secondaryTextSize
        DialogButtonBox.buttonRole: DialogButtonBox.RejectRole

        contentItem: Label {
            text: rejectButton.text
            color: "#c8cedb"
            font: rejectButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitHeight: 36
            implicitWidth: 96
            radius: 4
            color: rejectButton.down ? "#2a3140" : "transparent"
            border.width: rejectButton.activeFocus ? 2 : 1
            border.color: rejectButton.activeFocus ? box.focusRing : "#3a4353"
        }
    }

    Button {
        id: acceptButton
        objectName: "dialogAccept"
        text: box.acceptText
        enabled: box.acceptEnabled
        font.pixelSize: App.secondaryTextSize
        DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole

        contentItem: Label {
            text: acceptButton.text
            color: acceptButton.enabled ? "#ffffff" : "#8b93a7"
            font: acceptButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitHeight: 36
            implicitWidth: 96
            radius: 4
            color: !acceptButton.enabled
                   ? "#2a3140"
                   : acceptButton.down ? Qt.darker(box.actingColour, 1.3) : box.actingColour
            border.width: acceptButton.activeFocus ? 2 : 0
            border.color: "#ffffff"
        }
    }
}
