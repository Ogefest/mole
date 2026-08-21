import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What an operation is aimed at, listed by name.
//
// A count answers "how many"; the question in front of somebody about to delete
// something is "which ones". One component rather than one per dialog, so the
// compress dialog and the delete dialog cannot drift into disagreeing about how
// they say the same thing -- see docs/adr/0008-naming-what-an-operation-touches.md.
//
// Each row is { name, isDir, detail }, where detail is free text shown on the
// right: a size, a path, whatever the operation being confirmed makes relevant.
Rectangle {
    id: root

    property alias model: list.model
    /// How many rows are actually in it -- what is on screen, rather than what the
    /// caller believes it passed in.
    property alias count: list.count
    /// How tall it is allowed to get before it scrolls instead.
    property int maximumRows: 6

    implicitHeight: Math.min(App.listRowHeight * maximumRows,
                             Math.max(App.listRowHeight, list.contentHeight + 2))
    color: App.colour.pane
    border.color: App.colour.border
    border.width: 1
    radius: 3

    ListView {
        id: list
        objectName: "targetList"
        anchors.fill: parent
        anchors.margins: 1
        clip: true
        // Off outright when the rows fit, rather than left to work it out from a
        // height that is still settling.
        //
        // `implicitHeight` above chases `list.contentHeight`, which is zero until the
        // delegates exist -- so the box starts one row tall with a full model behind
        // it, the content overflows for those frames, and an AsNeeded scrollbar goes
        // active and then fades out again: a scrollbar appearing and vanishing in a
        // dialog that never needed one.
        //
        // It was *also* blamed for the guide's pictures of this dialog and the delete
        // one differing between runs, and that was wrong -- see MOLE-266. Those
        // differences are the 📄 glyph below rasterising differently, in two 7x10
        // patches whose 41-pixel span was mistaken for a scrollbar's height without
        // anybody looking at the pixels. This binding is still right on its own
        // terms, and the test on it asserts something that was genuinely false
        // before; it just never fixed what it was credited with.
        //
        // `list.count` comes from the model and does not wait for a layout, so the
        // answer is known before there is anything to get wrong. See MOLE-260.
        ScrollBar.vertical: ScrollBar {
            objectName: "targetListScrollBar"
            policy: list.count > root.maximumRows ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        delegate: RowLayout {
            required property var modelData
            width: ListView.view.width
            height: App.listRowHeight
            spacing: 6

            Label {
                Layout.leftMargin: 8
                // Glyphs from a scalable font, not the 📁 and 📄 that were here.
                //
                // Nothing on this machine has an outline for those codepoints: every
                // astral-plane emoji falls back to Unifont Upper, which is a *bitmap*
                // font, so the glyph is resampled to this pixel size. Something in the
                // render path flips between two states about one run in six -- the
                // cache or the atlas, and it was not worth chasing further -- and with
                // a resampled bitmap that flip costs 37 levels out of 255 in 116
                // pixels, which is enough to be seen and enough to rewrite two of the
                // guide's pictures. With DejaVu's outlines the same flip costs one
                // level, which is the ordinary noise ADR-0063 already lives with.
                // Measured over twenty runs apiece.
                //
                // ▤ is a page of lines and ▦ a grid of things; the kind is also in the
                // name, which carries a trailing slash for a folder. They are a
                // judgement call rather than a finding -- what is measured is the
                // font, not the choice of character. See MOLE-266.
                text: modelData.isDir ? "▦" : "▤"
                font.pixelSize: App.secondaryTextSize
            }
            Label {
                Layout.fillWidth: true
                text: modelData.isDir ? modelData.name + "/" : modelData.name
                elide: Text.ElideMiddle
                font.pixelSize: App.secondaryTextSize
            }
            Label {
                Layout.rightMargin: 8
                visible: text !== ""
                text: modelData.detail !== undefined ? modelData.detail : ""
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
