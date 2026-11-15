import QtQuick
import QtQuick.Controls

// The mark beside a symbolic link's icon, and the one beside a broken link's.
//
// A link keeps the icon of what it points at -- a link to a folder still reads as
// something you can open -- and gains this. Without it a link was drawn exactly
// like its target, so nothing on screen said that deleting or editing it touched
// a link rather than the file. See MOLE-454.
//
// Never the only signal: a broken link is also dimmed by the row that draws it,
// and the glyph differs as well as the colour (ADR-0010).
Label {
    property bool link: false
    property bool broken: false

    visible: link
    text: broken ? "⊘" : "↪"     // ⊘ points at nothing, ↪ points elsewhere
    font.pixelSize: App.smallTextSize
    font.bold: true
    color: broken ? App.colour.bad : App.colour.link
}
