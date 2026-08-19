import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Two lists, kept apart on purpose: drives are what the machine has,
// bookmarks are what the person cares about.
Rectangle {
    id: sidebar

    property color panelColor: "#1b2029"
    property color borderColor: "#2a3140"
    property color mutedText: "#8b93a7"

    color: panelColor

    /// The keyboard should go back to whatever the window was showing. A signal
    /// rather than a reach, because the sidebar has no business knowing that a
    /// file pane exists.
    signal focusWanted()

    // What a drive's state means, in the sidebar's own colours. The model says
    // what a state means -- "idle", "using", "broken" -- and the palette lives
    // here, so neither has to know the other's vocabulary.
    //
    // Hue is the *kind*: muted for nothing of yours, the accent for yours and in
    // use, red for broken. The accent is not a new colour -- it is what this
    // interface already means everywhere by "this is the thing you are on".
    //
    // The green that used to mean *connected* is gone. Under the model the dot
    // now follows, that state is Idle -- available and unused -- and a colour of
    // its own for it was a celebration of nothing happening. It also settles an
    // accessibility fault nobody filed: green against red is the one pair
    // deuteranopia cannot separate, and it was carrying connected against
    // unreachable. See docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    function stateColor(severity) {
        if (severity === "using")
            return Material.accent
        // Work going through. Green, and flickering rather than breathing -- see
        // the dot below and the 2026-08-19 revision in ADR-0052. Not a new colour:
        // it is the green this sidebar used to paint for *connected*, put to a job
        // it can actually do.
        if (severity === "working")
            return "#57ab5a"
        if (severity === "broken")
            return "#e5534b"
        return sidebar.mutedText
    }

    // Amber from about three quarters, red from about nine tenths: the point
    // of the colour is to be noticed before the disk is actually full.
    function fillColor(fraction) {
        if (fraction >= 0.9)
            return "#e5534b"
        if (fraction >= 0.75)
            return "#d9a441"
        return "#4f8cc9"
    }

    // A row in either list. No icons: a glyph nobody can decode is worse than
    // the name it replaced.
    component PlaceRow: ItemDelegate {
        id: row
        objectName: "placeRow"
        required property string label
        required property string target
        property bool removable: false
        // Capacity, when the drive has one. A bucket or an archive has no
        // meaningful size, and the row is simply a name in that case rather
        // than a bar showing a number nobody can stand behind.
        property bool capacityKnown: false
        property real capacityUsed: 0
        property string capacityFree: ""
        property string capacityTotal: ""
        signal removeRequested()

        // --- the drive rows use these; the bookmark rows leave them alone ---
        //
        // `actionable` rather than "connectable or ejectable", because those two
        // swap over the moment a drive connects. A control that comes and goes
        // takes its width out of the name beside it, and the name re-elides and
        // jumps -- the same fault the × was already fixed for.
        property bool actionable: false
        property bool connectable: false
        property bool ejectable: false
        property bool unlockable: false
        property string severity: ""
        // Shape and motion, each carrying one idea. Hollow against filled is *not
        // here yet* against *here* -- the pair the old grey conflated, and one a
        // shade of grey cannot express at eight pixels. Motion is *happening right
        // now*, and the word says which kind: `waiting` for an answer that has not
        // come back, `working` for something going through.
        property bool solidDot: true
        property string dotMoves: ""
        property string stateCaption: ""
        property string checkCaption: ""
        signal connectRequested()
        signal checkRequested()
        signal unlockRequested()

        width: ListView.view ? ListView.view.width : parent.width

        // A row is a button, so it gets a button's height. Thirty pixels read as
        // cramped and left no room around the × it has to contain, which is
        // App.minimumTarget tall on its own.
        //
        // Derived rather than typed in: the floor comes from the same target size
        // the button uses, and the rest from what the content actually needs, so
        // changing the type scale cannot leave a row too short for what is in it.
        readonly property int comfortableHeight: Math.max(App.minimumTarget + 8, App.listRowHeight)
        height: Math.max(rowContent.implicitHeight + topPadding + bottomPadding, comfortableHeight)

        // Tighter than the style's default 8, which on top of a row already tall
        // enough to hold a target-sized button made the list needlessly airy.
        topPadding: 4
        bottomPadding: 4

        // Navigating and then handing the keyboard back. Clicking a place in
        // order to look at it and being left unable to type into it is wrong on
        // its own terms, and every navigation control in the pane's own toolbar
        // already does exactly this.
        //
        // But only when it actually went there. A locked drive puts the passphrase
        // dialog up instead, and handing the keyboard to the listing behind a
        // modal takes it out of that modal for good: the popup is left holding no
        // focus, so nothing inside it can take the keyboard afterwards either.
        onClicked: {
            if (App.goTo(target))
                sidebar.focusWanted()
        }

        // Stated rather than inherited. Control.hoverEnabled follows a platform
        // style hint, so the highlight, the tooltip and the × all depended on
        // whatever the platform felt about hover effects -- and simply did not
        // happen where that hint is off.
        hoverEnabled: true

        ToolTip.visible: hovered
        ToolTip.text: {
            var lines = [target]
            if (row.stateCaption !== "")
                lines.push(row.stateCaption)
            if (capacityKnown)
                lines.push(Math.round(capacityUsed * 100) + "% used · "
                           + capacityFree + " free of " + capacityTotal)
            if (row.checkCaption !== "")
                lines.push(row.checkCaption)
            return lines.join("\n")
        }
        ToolTip.delay: 600

        // Flat, instant hover. The style's default fades in over ~200ms, which
        // reads as the list lagging behind the pointer.
        background: Rectangle {
            color: row.hovered ? "#262d3a" : "transparent"
        }

        contentItem: ColumnLayout {
            id: rowContent
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                // Fixed to the target size so a row is exactly as tall whether or
                // not it has a × in it. The drives list is mixed -- a local disk
                // cannot be ejected and an archive can -- and rows of two
                // different heights in one list read as a bug.
                Layout.preferredHeight: App.minimumTarget
                spacing: 4
                Rectangle {
                    id: stateDot
                    objectName: "placeStateDot"
                    // Absence already means something here: a bookmark row is not
                    // a drive and has no dot at all. Giving that to an idle drive
                    // would make two different kinds of row look identical, which
                    // is the fault this scheme exists to fix, inverted.
                    visible: row.severity !== ""
                    // The properties the appearance is made of, readable as
                    // themselves: a test asserts six appearances through this one
                    // objectName, and reading a colour tells it only a third of
                    // what the dot is saying.
                    property bool filled: row.solidDot
                    property string motion: row.dotMoves
                    property color hue: sidebar.stateColor(row.severity)
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: filled ? hue : "transparent"
                    border.width: filled ? 0 : 1.5
                    border.color: hue
                    Layout.alignment: Qt.AlignVCenter

                    // Two motions, and never both at once: a state either breathes
                    // or holds still. Both breathing states use the same shape and
                    // differ in how deep and how fast, because the difference being
                    // drawn is between *nothing has come back yet* and *this is
                    // going through*. Declared as animations with an explicit target
                    // rather than as `on opacity` value sources, because two value
                    // sources on one property fight over who owns it when neither is
                    // running.
                    SequentialAnimation {
                        running: stateDot.motion === "waiting" && stateDot.visible
                        loops: Animation.Infinite
                        alwaysRunToEnd: true
                        onRunningChanged: if (!running) stateDot.opacity = 1.0
                        NumberAnimation {
                            target: stateDot; property: "opacity"
                            to: 0.35; duration: 700; easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            target: stateDot; property: "opacity"
                            to: 1.0; duration: 700; easing.type: Easing.InOutQuad
                        }
                    }

                    // Work going through: the same breath, much shallower and half
                    // the speed. Enough to say something is happening, not enough to
                    // keep pulling the eye back to a corner of the window while
                    // somebody is doing something else. This was an uneven flicker
                    // first -- a disk activity light, taken literally -- and it was
                    // built, looked at beside the rows it has to live among, and
                    // withdrawn for reading as an alarm. See the second 2026-08-19
                    // revision in docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
                    SequentialAnimation {
                        running: stateDot.motion === "working" && stateDot.visible
                        loops: Animation.Infinite
                        alwaysRunToEnd: true
                        onRunningChanged: if (!running) stateDot.opacity = 1.0
                        NumberAnimation {
                            target: stateDot; property: "opacity"
                            to: 0.7; duration: 1000; easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            target: stateDot; property: "opacity"
                            to: 1.0; duration: 1000; easing.type: Easing.InOutQuad
                        }
                    }
                }
                Label {
                    objectName: "placeRowLabel"
                    Layout.fillWidth: true
                    text: row.label
                    elide: Text.ElideMiddle
                    font.pixelSize: App.textSize
                    // A drive nobody has connected is still a drive, and still
                    // worth listing -- but it is not somewhere you can go right
                    // now, and the row should not read as though it were.
                    color: row.actionable && row.connectable ? sidebar.mutedText
                                                             : Material.foreground
                }
                Label {
                    // Shown whether or not the pointer is here. It used to be
                    // hidden on hover to make room for the × -- but the × now
                    // keeps its place at all times, so there is nothing to make
                    // room for and nothing left to move.
                    visible: row.capacityKnown
                    text: row.capacityFree + " free"
                    color: sidebar.mutedText
                    font.pixelSize: App.smallTextSize
                }
                ToolButton {
                    objectName: "placeCheckButton"
                    // Only a configured drive can be checked: there is nothing
                    // to ask a local disk that the disk is not already
                    // answering by being there.
                    visible: row.actionable
                    opacity: row.hovered ? 1 : 0
                    enabled: row.hovered
                    text: "?"
                    font.pixelSize: App.textSize
                    implicitWidth: App.minimumTarget
                    implicitHeight: App.minimumTarget
                    onClicked: row.checkRequested()
                }
                ToolButton {
                    objectName: "placeRemoveButton"
                    // Always in the layout on a row that has one, and only faded
                    // in and out. Appearing on hover took its width out of the
                    // name beside it, so the name re-elided and visibly jumped
                    // as the pointer crossed the row -- twice, since the free
                    // caption was leaving at the same moment.
                    visible: row.removable || row.actionable
                    opacity: row.hovered ? 1 : 0
                    // A locked drive has a row and a state and no action: the
                    // answer is a passphrase, not a button that would fail.
                    enabled: row.hovered
                             && (row.removable || row.connectable || row.ejectable || row.unlockable)
                    // A locked drive gets a key rather than a play button: the
                    // thing standing between it and connecting is a passphrase,
                    // and pressing it goes to where that is typed rather than
                    // opening a second way to do the same thing.
                    text: row.unlockable ? "\u26bf"
                                         : (row.connectable ? "\u25b6" : (row.actionable ? "\u23cf" : "×"))
                    font.pixelSize: App.textSize
                    implicitWidth: App.minimumTarget
                    implicitHeight: App.minimumTarget
                    onClicked: {
                        if (row.unlockable)
                            row.unlockRequested()
                        else if (row.connectable)
                            row.connectRequested()
                        else
                            row.removeRequested()
                    }
                }
            }

            // The bar carries the proportion; the caption carries the numbers.
            // Neither alone answers "can I still copy this here".
            Rectangle {
                objectName: "capacityBar"
                Layout.fillWidth: true
                visible: row.capacityKnown
                implicitHeight: 4
                radius: 2
                color: "#141922"

                Rectangle {
                    width: Math.max(2, parent.width
                                       * Math.max(0, Math.min(1, row.capacityUsed)))
                    height: parent.height
                    radius: 2
                    color: sidebar.fillColor(row.capacityUsed)
                }
            }

            Label {
                visible: row.capacityKnown
                text: Math.round(row.capacityUsed * 100) + "% of " + row.capacityTotal
                color: sidebar.mutedText
                font.pixelSize: App.smallTextSize
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Label {
            text: "DRIVES"
            font.pixelSize: App.smallTextSize
            font.letterSpacing: 1
            color: sidebar.mutedText
        }

        // No band here any more. It asked for the passphrase at startup, for
        // drives nobody had opened yet -- the one moment nobody has a reason to
        // answer -- and it asked for it in a label, a field and a button sharing
        // about two hundred pixels of a sidebar that can be dragged down to a
        // hundred and sixty. The question is asked when a drive is opened now,
        // in a dialog. What stays here is the news: the row still reads Locked
        // and still offers the key rather than the play triangle. See
        // docs/adr/0031-a-locked-drive-is-connected-when-it-is-opened.md.

        // Local disks, network shares and mounted archives are the same kind
        // of row -- that uniformity is the point of the VFS layer.
        ListView {
            objectName: "driveList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, sidebar.height * 0.45)
            clip: true
            model: App.drives
            boundsBehavior: Flickable.StopAtBounds

            delegate: PlaceRow {
                required property int index
                required property string displayName
                required property string rootUri
                required property string scheme
                required property bool hasSpace
                required property real usedFraction
                required property string freeText
                required property string totalText
                required property bool canEject
                required property bool canConnect
                required property bool canUnlock
                required property string configuredId
                required property string stateText
                required property string stateSeverity
                required property bool dotFilled
                required property string dotMotion
                required property string checkMessage
                required property string checkedAt

                label: displayName
                target: rootUri
                capacityKnown: hasSpace
                capacityUsed: usedFraction
                capacityFree: freeText
                capacityTotal: totalText
                severity: stateSeverity
                solidDot: dotFilled
                dotMoves: dotMotion
                stateCaption: stateText

                // A drive somebody configured always has the slot, whatever it
                // is currently doing, so connecting one does not move anything.
                actionable: configuredId !== ""
                connectable: canConnect
                ejectable: canEject
                unlockable: canUnlock
                // Archives and other file-backed drives eject with the ×, as
                // they always have; the real disks stay.
                removable: canEject && scheme !== "file"

                checkCaption: checkedAt !== "" ? checkMessage + " · " + checkedAt : ""

                onConnectRequested: App.connectDrive(configuredId)
                // The same signal the palette's Unlock command raises, so the key
                // on a row and the command arrive at one dialog rather than at two
                // ways of doing the same thing.
                onUnlockRequested: App.requestCredentials()
                onCheckRequested: App.checkDrive(configuredId)
                onRemoveRequested: configuredId !== "" ? App.disconnectDrive(configuredId)
                                                       : App.drives.unmount(index)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 6
            implicitHeight: 1
            color: sidebar.borderColor
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: "BOOKMARKS"
                font.pixelSize: App.smallTextSize
                font.letterSpacing: 1
                color: sidebar.mutedText
            }
            ToolButton {
                objectName: "addBookmarkButton"
                text: "+"
                font.pixelSize: App.textSize
                implicitWidth: App.minimumTarget
                implicitHeight: App.minimumTarget
                ToolTip.visible: hovered
                ToolTip.text: "Add the current folder  (Ctrl+D)"
                onClicked: App.triggerAction("mole.bookmarks.add")
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: App.bookmarks
            boundsBehavior: Flickable.StopAtBounds

            delegate: PlaceRow {
                required property int index
                required property string name
                required property string uri

                label: name
                target: uri
                removable: true
                onRemoveRequested: App.bookmarks.removeAt(index)
            }

            Label {
                anchors.centerIn: parent
                visible: App.bookmarks.count === 0
                width: parent.width - 16
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "No bookmarks yet.\nCtrl+D adds the folder you are in."
                color: "#5c6472"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
