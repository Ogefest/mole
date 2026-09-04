import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// Every dialog in Mole, so that none of them can be missing a line.
//
// **Nineteen dialogs pasted the same nine lines** -- the panel ground, both
// overlay veils, `modal`, `focus`, `anchors.centerIn`, a width, and six lines of
// comment explaining the first three. Three CMake greps stand in for the
// component that makes the omission impossible, and those greps exist because the
// paste was forgotten three times.
//
// The placement drifted with it, too: six dialogs centred on `parent` -- the tab
// body, which excludes the sidebar and, with the terminal panel open, the lower
// third of the window -- and nine on `Overlay.overlay`. Four fixed a width with
// no clamp at 520 px while five clamped, so a narrow window cut one dialog off
// and not the next.
//
// What a dialog still says for itself: its title, its footer's accept text, what
// it holds, and what happens when it is accepted. See MOLE-398.
Dialog {
    id: base

    /// How wide, at most. Clamped to the window so a narrow one is never cut off.
    property int preferredWidth: 400

    // A dialog sits on the panel ground, said here rather than inherited: the
    // window no longer hands one down. See ADR-0074.
    Material.background: App.colour.panel
    // Dimmed rather than washed out: Qt's Material dark theme dims with
    // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
    Overlay.modal: DimVeil {}
    Overlay.modeless: DimVeil {}

    modal: true
    // Without this the popup never becomes a focus scope, so nothing inside it
    // can take the keyboard and Tab walks the window behind instead.
    focus: true

    // On the window's overlay rather than on whatever declared it: centred on a
    // parent, a dialog lands in the middle of the tab body -- which is not the
    // middle of the window once there is a sidebar, and is a third of the way up
    // it with the terminal panel open.
    anchors.centerIn: Overlay.overlay
    width: Math.min(preferredWidth, Overlay.overlay ? Overlay.overlay.width - 48 : preferredWidth)
}
