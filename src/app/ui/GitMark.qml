import QtQuick
import QtQuick.Controls

// One git letter, coloured: `M`, `A`, `D`, `??`, `R`, `U`, or the dot that
// stands for something below a folder.
//
// Its own file because two places draw it -- the row in a listing and the entry
// in the band's list of changed paths -- and a second copy of the table is a
// second chance for the two to disagree about what a `D` looks like.
//
// The colour is keyed off the letter rather than off the bitmask, so it cannot
// disagree with the mark either: the letter has already resolved "several states
// at once" to the one worth showing. Never the only signal -- the letter carries
// the meaning on its own, see ADR-0010.
Label {
    property string mark: ""

    text: mark
    horizontalAlignment: Text.AlignHCenter
    font.pixelSize: App.smallTextSize
    font.bold: true
    color: switch (mark) {
           case "U": return "#e5534b"   // conflicted
           case "D": return "#e5534b"   // deleted
           case "A": return "#5fb977"   // added
           case "R": return "#7cc4ff"   // renamed
           case "M": return "#d9a441"   // modified
           case "??": return "#8b93a7"  // untracked
           default: return "#6f7788"    // something inside
           }
}
