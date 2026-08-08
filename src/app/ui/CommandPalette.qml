import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One box that reaches everything: the menu, the bookmarks, the drives.
//
// Opened with a key and driven entirely from the keyboard, because the reason it
// exists is that not every control has a shortcut of its own -- so it would be an
// odd sort of answer if it needed the mouse.
Popup {
    id: palette
    objectName: "commandPalette"

    readonly property var commands: App.commands

    width: Math.min(720, parent ? parent.width - 80 : 720)
    // Sized to what is in it, up to a ceiling: one match should not leave a tall
    // empty box below it, and a hundred should not fill the window.
    height: Math.min(Math.min(460, parent ? parent.height - 120 : 460),
                     field.height + 17 + Math.max(App.listRowHeight, list.contentHeight))
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round(parent.height * 0.12) : 0
    modal: true
    focus: true
    padding: 0

    background: Rectangle {
        color: "#1b2029"
        border.color: "#2a3140"
        border.width: 1
        radius: 4
    }

    onAboutToShow: {
        // Rebuilt every time: what can be done depends on the tab in front of the
        // user, and a stale list would offer things that no longer apply.
        palette.commands.filter = ""
        palette.commands.refresh()
        list.currentIndex = 0
    }
    onOpened: field.forceActiveFocus()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TextField {
            id: field
            objectName: "commandPaletteInput"
            Layout.fillWidth: true
            Layout.margins: 8
            placeholderText: "Type to find a command, a bookmark or a drive…"
            font.pixelSize: App.textSize
            selectByMouse: true
            onTextChanged: {
                palette.commands.filter = text
                list.currentIndex = 0
            }

            // The list is driven from here, so the keyboard never has to leave the
            // box being typed into.
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onReturnPressed: palette.run()
            Keys.onEnterPressed: palette.run()
            Keys.onEscapePressed: palette.close()
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: "#2a3140"
        }

        ListView {
            id: list
            objectName: "commandPaletteList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: palette.commands
            currentIndex: 0
            highlightMoveDuration: 0
            keyNavigationEnabled: false // the field owns the arrows

            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                required property int index
                required property string title
                required property string group
                required property string shortcut
                required property string iconText

                width: ListView.view.width
                height: App.listRowHeight
                highlighted: ListView.isCurrentItem
                onClicked: {
                    list.currentIndex = index
                    palette.run()
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Label {
                        Layout.preferredWidth: 20
                        text: iconText
                        color: Material.accent
                        font.pixelSize: App.secondaryTextSize
                    }
                    Label {
                        text: title
                        font.pixelSize: App.textSize
                    }
                    // Where it came from, so the row answers "which Refresh is
                    // this" without being read as part of the name.
                    Label {
                        Layout.fillWidth: true
                        text: group
                        color: "#6f7788"
                        font.pixelSize: App.smallTextSize
                        elide: Text.ElideRight
                    }
                    Label {
                        visible: shortcut.length > 0
                        text: shortcut
                        color: "#8b93a7"
                        font.family: App.monospaceFont
                        font.pixelSize: App.smallTextSize
                    }
                }
            }

            // Said plainly, because an empty list with no explanation reads as a
            // broken search rather than as no matches.
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "Nothing matches “" + field.text + "”"
                color: "#6f7788"
                font.pixelSize: App.secondaryTextSize
            }
        }
    }

    function run() {
        if (list.count === 0)
            return
        const row = Math.max(0, list.currentIndex)
        palette.close()
        // Closed first: an action that opens a tab or a dialog should not have to
        // fight a popup that is still on top of it.
        palette.commands.activate(row)
    }
}
