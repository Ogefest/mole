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

        onClicked: App.goTo(target)

        // Stated rather than inherited. Control.hoverEnabled follows a platform
        // style hint, so the highlight, the tooltip and the × all depended on
        // whatever the platform felt about hover effects -- and simply did not
        // happen where that hint is off.
        hoverEnabled: true

        ToolTip.visible: hovered
        ToolTip.text: capacityKnown
                      ? target + "\n" + Math.round(capacityUsed * 100) + "% used · "
                        + capacityFree + " free of " + capacityTotal
                      : target
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
                Label {
                    objectName: "placeRowLabel"
                    Layout.fillWidth: true
                    text: row.label
                    elide: Text.ElideMiddle
                    font.pixelSize: App.textSize
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
                    objectName: "placeRemoveButton"
                    // Always in the layout on a row that has one, and only faded
                    // in and out. Appearing on hover took its width out of the
                    // name beside it, so the name re-elided and visibly jumped
                    // as the pointer crossed the row -- twice, since the free
                    // caption was leaving at the same moment.
                    visible: row.removable
                    opacity: row.hovered ? 1 : 0
                    enabled: row.hovered
                    text: "×"
                    font.pixelSize: App.textSize
                    implicitWidth: App.minimumTarget
                    implicitHeight: App.minimumTarget
                    onClicked: row.removeRequested()
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

                label: displayName
                target: rootUri
                capacityKnown: hasSpace
                capacityUsed: usedFraction
                capacityFree: freeText
                capacityTotal: totalText
                // Archives and other file-backed drives can be ejected; the
                // real disks stay, and a configured drive that is not connected
                // has nothing to eject yet.
                removable: canEject && scheme !== "file"
                onRemoveRequested: App.drives.unmount(index)
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
