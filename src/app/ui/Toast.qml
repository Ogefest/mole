import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A message that appears, is read, and goes away by itself.
//
// **The keyboard is the whole reason this is one component.** A Popup with the
// default `closePolicy` closes on Escape, which tells Qt Quick Controls it wants
// key events -- and while an overlay popup wants them, every window `Shortcut`
// outside it stops working. The window's own toast has carried a comment about
// that since MOLE-128; the browser view's copy of the same toast did not get the
// line, so after any failed operation -- a refused rename, a drop with nowhere to
// go, "no drive is mounted here" -- F3, Ctrl+W, Ctrl+T, Ctrl+R and F4 were dead
// for six seconds, at the moment somebody most wants them. Two toasts, two
// timers, two positions, and the bug in one of them. See MOLE-398.
//
// Dismissed by clicking outside it, which still works.
Popup {
    id: toast

    /// How far above the bottom edge of whatever it is in. Not `bottomMargin`,
    /// which is Popup's own and FINAL.
    property int liftBy: 40
    /// How long it stays. A notice with something to press sets this to 0 and
    /// waits to be dismissed: a button that disappears while somebody reaches
    /// for it is worse than no button.
    property int dwellMs: 5000
    /// The colour of the words. A failure says so.
    property alias textColour: label.color
    property alias text: label.text
    /// Anything to put beside the words -- a button, usually nothing.
    default property alias trailing: trailingRow.data
    /// Whether the dwell is running. Asked by the suite instead of reaching for
    /// the timer inside: two toasts exist and their innards have the same names.
    readonly property alias counting: dwell.running

    // Dimmed rather than washed out: Qt's Material dark theme dims with
    // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
    Overlay.modal: DimVeil {}
    Overlay.modeless: DimVeil {}

    closePolicy: Popup.CloseOnPressOutside

    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? parent.height - height - liftBy : 0
    width: parent ? Math.min(600, parent.width - 60) : 600
    padding: 14
    Material.background: App.colour.panel

    /// Shows `message`, restarting the dwell if one is already up.
    function show(message) {
        label.text = message
        toast.open()
    }

    Timer {
        id: dwell
        objectName: "toastTimer"
        running: toast.opened && toast.dwellMs > 0
        interval: toast.dwellMs
        onTriggered: toast.close()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            id: label
            objectName: "toastLabel"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        RowLayout {
            id: trailingRow
            spacing: 8
        }
    }
}
