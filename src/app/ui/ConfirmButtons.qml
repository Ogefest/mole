import QtQuick
import QtQuick.Controls

// The ways out of a dialog, told apart at a glance.
//
// The standard buttons are two flat labels of the same weight and colour, so "Yes"
// and "No" -- or worse, "Ok" and "Cancel" over something irreversible -- read as one
// undifferentiated pair, and neither of them holds the keyboard. This says which one
// acts, what it will do, and where the keyboard is.
//
// The rules are in docs/adr/0010-telling-the-two-buttons-apart.md:
//   - the button that acts is filled; the one that backs out stays outlined;
//   - it is filled red when what it does cannot be undone;
//   - the keyboard starts on the acting button, so a question can be answered with
//     Return -- unless the answer cannot be undone, when it starts on the way out,
//     or the dialog is typed into, when the field wins;
//   - a dialog with only something to say has one button, and it still holds the
//     keyboard: with nothing focused there is no focus ring and Return does nothing.
//
// Every dialog in the window uses this, and `standardButtons` is refused at configure
// time -- see src/app/CMakeLists.txt. ADR-0010 predicted in writing that the next
// dialog would go back to two identical labels, and three of them did.
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
    property string rejectText: dismissOnly ? "Close" : "Cancel"
    /// Irreversible: the acting button turns red and the keyboard starts on the
    /// other one.
    property bool destructive: false
    property bool acceptEnabled: true
    /// A dialog that only has something to say -- a list of shortcuts, a hint,
    /// a form that saves as you go -- has one way out and nothing to tell apart.
    /// The acting button goes; the keyboard does not.
    property bool dismissOnly: false
    /// Where the keyboard starts: "accept", "reject", or "none" for a dialog that
    /// puts it in a field of its own. Set it to "none" and focus the field, never
    /// leave it nowhere: a dialog with the keyboard nowhere cannot be answered
    /// from the keyboard at all.
    property string keyboardOn: (dismissOnly || destructive) ? "reject" : "accept"

    readonly property color actingColour: destructive ? "#c0392b" : "#2d6cdf"
    readonly property color focusRing: "#9db4ff"
    readonly property bool holdsKeyboard: keyboardOn !== "none"

    // Three scopes deep, and every one of them has to be holding the keyboard for the
    // button at the bottom to have it: the popup, this box, and the ListView the box
    // lays its buttons out with. Miss the middle one and the focus is set, reported as
    // set, and never active.
    focus: holdsKeyboard

    Binding {
        target: box.contentItem
        property: "focus"
        value: box.holdsKeyboard
    }

    // And then said again, imperatively. `focus: true` on the right button is not
    // enough: the box lays its buttons out with a ListView, and a ListView hands
    // the keyboard to its own current item -- whichever button ends up first --
    // so the declarative answer was quietly overruled and every dialog opened on
    // the way out, including the ones where that is the wrong button.
    function placeKeyboard() {
        if (box.keyboardOn === "reject")
            rejectButton.forceActiveFocus()
        else if (box.keyboardOn === "accept" && acceptButton.visible)
            acceptButton.forceActiveFocus()
        // "none" deliberately does nothing: the dialog is focusing a field of its own.
    }

    onVisibleChanged: if (visible) Qt.callLater(placeKeyboard)
    Component.onCompleted: if (visible) Qt.callLater(placeKeyboard)
    // Choosing the irreversible answer while the dialog is open moves the keyboard
    // off the acting button, which is a steal on purpose.
    onKeyboardOnChanged: if (visible) Qt.callLater(placeKeyboard)

    Button {
        id: rejectButton
        objectName: "dialogReject"
        text: box.rejectText
        flat: true
        focus: box.keyboardOn === "reject"
        // A focused Button answers Space and not Return: Return is a Dialog-level
        // key and QQuickDialog only turns it into an answer when a standard button
        // is in charge, which is the thing this footer replaced. Without these two
        // lines the focus ring is on a button that Return does not press.
        Keys.onReturnPressed: rejectButton.clicked()
        Keys.onEnterPressed: rejectButton.clicked()
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
        visible: !box.dismissOnly
        focus: box.keyboardOn === "accept"
        Keys.onReturnPressed: acceptButton.clicked()
        Keys.onEnterPressed: acceptButton.clicked()
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
