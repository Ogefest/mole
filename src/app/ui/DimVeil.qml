import QtQuick

/// The veil behind a popup, in a colour that dims rather than washes.
///
/// Qt's Material style dims with `Material.backgroundDimColor`, and for a dark
/// theme that is `#99fafafa` -- near-white at sixty percent. Over this
/// interface's own `#151922` it comes out at `(158, 160, 164)`: the sidebar and
/// the listing behind a dialog go pale grey, labels read as half transparent,
/// and the whole window looks like a fade that stopped part way. It is not a
/// scrim, it is a wash, and it is what MOLE-128 was about.
///
/// `backgroundDimColor` is read-only, and the style sets `Overlay.modal` on each
/// control rather than on the window -- so the window-level fallback is never
/// reached and every popup has to say this for itself. **A new dialog, popup or
/// menu declares `Overlay.modal: DimVeil {}` and `Overlay.modeless: DimVeil {}`,
/// or it brings the wash back.** tst_Walkthrough asserts the pixel.
///
/// Black at thirty-two percent, which is the scrim Material's own specification
/// asks for, with the style's fade kept: a dark interface is dimmed by darkening
/// it. The window behind stays recognisably itself and the popup is the lit thing
/// on the screen, which is the whole job of a scrim.
Rectangle {
    color: "#52000000"

    Behavior on opacity {
        NumberAnimation { duration: 150 }
    }
}
