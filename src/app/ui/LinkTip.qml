import QtQuick
import QtQuick.Controls

// Where a symbolic link points, shown on hover as `→ <target>`.
//
// Read from the drive when the pointer arrives and not while listing: a folder of
// five thousand links must not pay a readlink per link to draw itself, and on a
// remote drive each one is a round trip. The controller asks off the thread that
// draws the window and the tip fills in when the answer comes back. See MOLE-454.
//
// Lays itself over the row it is put in, and takes no clicks: the row's own press,
// drag and double click go on working underneath.
Item {
    id: tip
    objectName: "linkTip"

    property var controller: null
    property string uri: ""
    property bool link: false

    property string target: ""
    property bool asked: false

    anchors.fill: parent

    HoverHandler {
        id: hover
        enabled: tip.link
        onHoveredChanged: {
            if (hovered && !tip.asked && tip.controller) {
                tip.asked = true
                tip.controller.readLinkTarget(tip.uri)
            }
        }
    }

    // Only listening while asked, so a screenful of rows is not a screenful of
    // handlers woken by every answer.
    Connections {
        target: tip.controller
        enabled: tip.asked && tip.target.length === 0
        function onLinkTargetRead(uri, answer, error) {
            if (uri === tip.uri)
                tip.target = answer.length > 0 ? answer : error
        }
    }

    // A row reused for another entry asks again rather than showing the last one's.
    onUriChanged: { asked = false; target = "" }

    ToolTip.visible: link && hover.hovered
    ToolTip.delay: 500
    ToolTip.text: "→ " + (target.length > 0 ? target : "…")
}
