import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The one action a view exists to perform: Search, Scan, Preview, Apply, Save.
//
// Its own control rather than `Button { highlighted: true }`, which was right on a
// dark window by accident. Material paints a highlighted button's label white
// whatever the theme, and takes its fill from the background the button
// *inherited* -- so under a light palette the primary action in six views came out
// white on white, and under a dark one the fill happened to match the toolbar and
// the affordance was the drop shadow alone.
//
// Filled in the accent and labelled in `window`, which is the token on the far
// side of the polarity from the accent on every theme: light text on a dark
// accent when the window is light, dark text on a light accent when it is dark.
// `tst_Palette` holds that as a ratio rather than a hope. See ADR-0074.
Button {
    id: control

    /// False for the same button in the state where it is not the thing to do --
    /// a Stop that interrupts what the fill was inviting. Outlined rather than
    /// filled, and still the same size, so the row does not move.
    property bool filled: true

    font.pixelSize: App.secondaryTextSize

    contentItem: Label {
        text: control.text
        color: !control.enabled ? App.colour.textMuted
                                : (control.filled ? App.colour.window : App.colour.text)
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitHeight: 34
        implicitWidth: 92
        radius: 4
        color: {
            if (!control.enabled)
                return App.colour.border
            if (!control.filled)
                return control.down ? App.colour.hover : "transparent"
            return control.down ? Qt.darker(App.colour.accent, 1.3) : App.colour.accent
        }
        border.width: control.activeFocus ? 2 : (control.filled ? 0 : 1)
        border.color: control.activeFocus ? App.colour.window : App.colour.border
    }
}
