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
        height: capacityKnown ? 46 : 30
        onClicked: App.goTo(target)
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
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    text: row.label
                    elide: Text.ElideMiddle
                    font.pixelSize: 13
                }
                Label {
                    visible: row.capacityKnown && !row.hovered
                    text: row.capacityFree + " free"
                    color: sidebar.mutedText
                    font.pixelSize: 10
                }
                ToolButton {
                    visible: row.removable && row.hovered
                    text: "×"
                    implicitWidth: 20
                    implicitHeight: 20
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
                font.pixelSize: 10
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Label {
            text: "DRIVES"
            font.pixelSize: 11
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
            model: App.mounts
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

                label: displayName
                target: rootUri
                capacityKnown: hasSpace
                capacityUsed: usedFraction
                capacityFree: freeText
                capacityTotal: totalText
                // Archives and other file-backed drives can be ejected; the
                // real disks stay.
                removable: scheme !== "file"
                onRemoveRequested: App.mounts.unmount(index)
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
                font.pixelSize: 11
                font.letterSpacing: 1
                color: sidebar.mutedText
            }
            ToolButton {
                text: "+"
                implicitWidth: 22
                implicitHeight: 22
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
                font.pixelSize: 11
            }
        }
    }
}
