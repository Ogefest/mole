import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What git says about the folder in view.
//
// A strip inside the pane rather than a panel or a dialog: this is a fact about
// one folder, and a dual view has two of them saying different things about two
// checkouts at once.
//
// Absent rather than empty when there is nothing to say. Being invisible is what
// does that -- a layout leaves an invisible item out instead of reserving its
// height -- so a folder that is not a checkout looks the way it looked before any
// of this existed, to the pixel. No "not a repository" text, no empty strip.
Rectangle {
    id: band

    // The pane's RepositoryInfo. Null before a pane is attached, which is a state
    // QML really passes through while the window is being built.
    property var info: null

    objectName: "repositoryBand"
    visible: info !== null && info.present
    implicitHeight: visible ? contents.implicitHeight + 10 : 0
    Layout.fillWidth: true
    color: "#1b2029"
    border.color: "#2a3140"
    border.width: 1
    radius: 3

    RowLayout {
        id: contents
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        // The glyph rather than the word "branch". A pane is narrow, and this is
        // the mark anybody with a checkout already reads without thinking.
        Label {
            text: "⎇"
            color: "#6f7788"
            font.pixelSize: App.textSize
        }

        Label {
            objectName: "repositoryHead"
            Layout.maximumWidth: band.width / 2
            text: band.info ? band.info.headText : ""
            elide: Text.ElideMiddle
            // Amber while git is part-way through something, because that is a
            // state somebody has to come back out of. Never the only signal: the
            // label says "rebasing" in words -- see ADR-0010.
            color: band.info && band.info.stateText.length > 0 ? "#d9a441" : "#7cc4ff"
            font.pixelSize: App.secondaryTextSize
            font.bold: true
        }

        Item { Layout.fillWidth: true }
    }
}
