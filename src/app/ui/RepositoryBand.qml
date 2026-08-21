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

    // A changed path the reader picked out of the list. `deleted` is the half the
    // pane cannot work out for itself: the file is not there, so going to it means
    // going to the folder that held it. Answered by whoever owns the pane rather
    // than acted on here, because this strip knows about one repository and
    // nothing about navigating.
    signal pathActivated(string uri, bool deleted)

    objectName: "repositoryBand"
    visible: info !== null && info.present
    implicitHeight: visible ? contents.implicitHeight + 10 : 0
    Layout.fillWidth: true
    color: App.colour.panel
    border.color: App.colour.border
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
            color: App.colour.textFaint
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
            color: band.info && band.info.stateText.length > 0 ? App.colour.warn : App.colour.link
            font.pixelSize: App.secondaryTextSize
            font.bold: true
        }

        // How much of the work tree differs from the last commit. Absent until the
        // walk answers -- the branch is cheap and arrives first, and a band that
        // said "clean" for the moment before the walk landed would be telling a
        // lie that reads exactly like the truth.
        //
        // Something to open rather than only to read, once there is anything to
        // count. It is the only way to reach a file git has been told to delete: a
        // listing shows what is on disk, so a deletion has no row to carry its
        // letter. See ADR-0042.
        Label {
            id: changesLabel
            objectName: "repositoryChanges"

            // "clean" is a fact and not a door. Opening an empty list would be a
            // control that does nothing, which is worse than no control.
            readonly property bool openable: band.info !== null && band.info.changedCount > 0

            visible: text.length > 0
            text: band.info ? band.info.changesText : ""
            color: band.info && band.info.changedCount > 0 ? App.colour.textSecondary : App.colour.textFaint
            font.pixelSize: App.secondaryTextSize
            // Underlined while the pointer is on it, so it reads as something to
            // press before it is pressed. Not underlined always: the band is a strip
            // of facts and one permanently underlined word among them reads as an
            // error rather than as a link.
            font.underline: changesLabel.openable && changesHover.hovered

            HoverHandler {
                id: changesHover
                enabled: changesLabel.openable
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler {
                enabled: changesLabel.openable
                onTapped: changedPaths.open()
            }
        }

        // How far the branch is from what it tracks, as it was last fetched -- nothing
        // here talks to a network, so the words are "2 ahead, 1 behind" rather than
        // anything implying a check just happened.
        Label {
            objectName: "repositoryTracking"
            visible: text.length > 0
            text: band.info ? band.info.trackingText : ""
            color: App.colour.warn
            font.pixelSize: App.secondaryTextSize
        }

        // The commit HEAD is on: which one, what it was, and how long ago. The subject
        // is the part that gives way -- it takes the space that is left and elides,
        // rather than wrapping the band to two lines.
        Label {
            objectName: "repositoryCommitId"
            visible: band.info ? band.info.hasCommit : false
            text: band.info ? band.info.shortId : ""
            color: App.colour.textFaint
            font.pixelSize: App.secondaryTextSize
        }

        Label {
            objectName: "repositoryCommitSubject"
            Layout.fillWidth: true
            text: band.info && band.info.hasCommit ? band.info.commitSubject : ""
            elide: Text.ElideRight
            color: App.colour.textMuted
            font.pixelSize: App.secondaryTextSize
        }

        Label {
            objectName: "repositoryCommitAge"
            visible: band.info ? band.info.hasCommit : false
            text: band.info ? band.info.commitAge : ""
            color: App.colour.textFaint
            font.pixelSize: App.secondaryTextSize
        }
    }

    // What the count is counting, one path per row.
    //
    // A popup rather than a second strip: this is something somebody asks for and
    // then closes again, and a band that grew to fifteen lines would take that
    // height off the listing for as long as the checkout was dirty.
    Popup {
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

        id: changedPaths
        objectName: "repositoryChangedPaths"

        // Under the word it belongs to, and pulled back inside the pane when the
        // word is near the right edge -- a pane is narrow and this is wider than it.
        x: Math.max(0, Math.min(changesLabel.x, band.width - width))
        y: band.height + 2
        width: Math.min(460, band.width)
        // Tall enough for a handful of paths, and no taller: past that it scrolls,
        // because a popup as tall as the window hides the listing it is about.
        implicitHeight: Math.min(pathList.contentHeight + 2 * padding, 320)
        padding: 6
        Material.background: App.colour.panel

        // Anything that navigates has taken the reader out of this folder, so the
        // list has nothing left to be about.
        Connections {
            target: band.info
            function onChanged() {
                if (!band.info || band.info.changedCount === 0)
                    changedPaths.close()
            }
        }

        ListView {
            id: pathList
            anchors.fill: parent
            clip: true
            model: band.info ? band.info.changedPaths : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                required property var modelData

                // The mark and the path as one string. What a test asserts on:
                // reading the two labels separately and zipping them would depend
                // on what order the delegates happened to be built in.
                readonly property string entryText: modelData.mark + " " + modelData.path

                objectName: "repositoryChangedPath"
                width: ListView.view.width
                height: App.listRowHeight
                padding: 4

                contentItem: RowLayout {
                    spacing: 6

                    GitMark {
                        Layout.preferredWidth: 22
                        mark: modelData.mark
                    }

                    // Relative to the work tree root: a reader looking at one
                    // checkout already knows where it is, and the absolute path
                    // would be mostly prefix repeated on every row. Elided at the
                    // front, because what tells two of these apart is the end.
                    Label {
                        objectName: "repositoryChangedPathText"
                        Layout.fillWidth: true
                        text: modelData.path
                        elide: Text.ElideLeft
                        // Struck through when the file is not there, which is the
                        // one row in this list that behaves differently when it is
                        // activated. The letter `D` says it too.
                        font.strikeout: modelData.deleted
                        color: modelData.deleted ? App.colour.textMuted : App.colour.textSecondary
                        font.pixelSize: App.secondaryTextSize
                    }
                }

                onClicked: {
                    changedPaths.close()
                    band.pathActivated(modelData.uri, modelData.deleted)
                }
            }
        }
    }
}
