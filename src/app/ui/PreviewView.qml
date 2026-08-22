import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The preview tab. It owns which file is shown and the arrows that step
// through the folder; what the file looks like comes entirely from whichever
// viewer the registry picked, loaded below.
Item {
    id: view

    property var controller: null

    function focusActivePane() { body.forceActiveFocus() }

    Component.onCompleted: Qt.callLater(focusActivePane)
    onVisibleChanged: if (visible) Qt.callLater(focusActivePane)

    FocusScope {
        id: body
        anchors.fill: parent

        // Left and right step through the folder. They are the reason this is
        // a tab rather than a dialog.
        Keys.onLeftPressed: if (controller) controller.previous()
        Keys.onRightPressed: if (controller) controller.next()
        Keys.onSpacePressed: if (controller) controller.next()
        Keys.onBackPressed: if (controller) controller.previous()

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            ToolBar {
                Layout.fillWidth: true
                Material.background: App.colour.panel

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 6

                    ToolButton {
                        text: "◀"
                        font.pixelSize: App.textSize
                        enabled: controller ? controller.canGoPrevious : false
                        focusPolicy: Qt.NoFocus
                        onClicked: { controller.previous(); body.forceActiveFocus() }
                    }
                    ToolButton {
                        text: "▶"
                        font.pixelSize: App.textSize
                        enabled: controller ? controller.canGoNext : false
                        focusPolicy: Qt.NoFocus
                        onClicked: { controller.next(); body.forceActiveFocus() }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: controller ? controller.fileName : ""
                        elide: Text.ElideMiddle
                        font.pixelSize: App.textSize
                        font.bold: true
                    }

                    Label {
                        visible: controller && controller.siblingCount > 0
                        text: controller
                              ? controller.position + " of " + controller.siblingCount : ""
                        color: App.colour.textMuted
                        font.pixelSize: App.secondaryTextSize
                    }

                    // Which state of the file is on screen, said whether or not
                    // the drive has anything else to offer. A preview showing an
                    // earlier version while looking like the file itself is the
                    // one failure this subject has to avoid, so the label is not
                    // conditional on there being a choice.
                    Label {
                        objectName: "versionLabel"
                        text: controller && controller.showingVersion.length > 0
                              ? controller.showingVersion : "current"
                        color: controller && controller.showingVersion.length > 0
                               ? App.colour.accent : App.colour.textMuted
                        font.pixelSize: App.secondaryTextSize
                        font.bold: controller && controller.showingVersion.length > 0
                    }

                    // Only when the drive has other states of this file, and the
                    // list itself is not fetched until this is opened: asking is
                    // a call into storage and opening a preview must not make one
                    // nobody wanted.
                    Picker {
                        objectName: "versionPicker"
                        visible: controller ? controller.hasOtherVersions : false
                        font.pixelSize: App.secondaryTextSize
                        focusPolicy: Qt.NoFocus
                        textRole: "label"

                        readonly property var entries: {
                            var rows = [{ "uri": "", "label": "current" }]
                            if (controller) {
                                var others = controller.otherVersions
                                for (var i = 0; i < others.length; ++i)
                                    rows.push(others[i])
                            }
                            return rows
                        }

                        model: entries
                        currentIndex: {
                            if (!controller || controller.showingVersion.length === 0)
                                return 0
                            for (var i = 1; i < entries.length; ++i) {
                                if (entries[i].label === controller.showingVersion)
                                    return i
                            }
                            return 0
                        }

                        onPressedChanged: if (pressed && controller) controller.requestVersions()
                        onActivated: if (controller) controller.showVersion(entries[currentIndex].uri)
                    }

                    Label {
                        objectName: "versionsError"
                        visible: controller && controller.versionsError.length > 0
                        text: controller ? controller.versionsError : ""
                        color: App.colour.bad
                        font.pixelSize: App.smallTextSize
                        elide: Text.ElideRight
                        Layout.maximumWidth: 220
                    }

                    ToolSeparator {}

                    Label {
                        text: controller ? controller.viewerName : ""
                        color: Material.accent
                        font.pixelSize: App.smallTextSize
                    }

                    // Next to the viewer's name, because it is the reason that
                    // name is not the one that first claimed the file. The colour
                    // of a caveat rather than of an error: nothing went wrong with
                    // the file, and what is on screen is the honest second answer.
                    Label {
                        objectName: "fallbackNote"
                        visible: controller && controller.fallbackNote.length > 0
                        text: controller ? controller.fallbackNote : ""
                        color: App.colour.warn
                        font.pixelSize: App.smallTextSize
                        elide: Text.ElideRight
                        Layout.maximumWidth: 320
                    }

                    // Whatever this viewer says can be chosen about how it shows
                    // this file. The strip renders these without knowing what any
                    // of them mean, the same way the menu renders entries from
                    // plugins it has never heard of.
                    Repeater {
                        model: controller ? controller.viewerOptions : []

                        delegate: RowLayout {
                            required property var modelData
                            spacing: 4

                            Label {
                                text: modelData.title
                                color: App.colour.textMuted
                                font.pixelSize: App.smallTextSize
                            }
                            Picker {
                                objectName: "viewerOption_" + modelData.key
                                font.pixelSize: App.secondaryTextSize
                                focusPolicy: Qt.NoFocus
                                model: modelData.choices
                                currentIndex: modelData.choices.indexOf(modelData.chosen)
                                onActivated: controller.chooseViewerOption(modelData.key, currentText)
                            }
                        }
                    }

                    // The drawer's switch, beside the viewer's own choices --
                    // the Source/Rendered picker on an .html is its neighbour.
                    CheckBox {
                        objectName: "detailsToggle"
                        text: "Details"
                        font.pixelSize: App.secondaryTextSize
                        focusPolicy: Qt.NoFocus
                        checked: controller ? controller.detailsOpen : false
                        onToggled: if (controller) controller.setDetailsOpen(checked)
                    }

                    ToolButton {
                        text: "Open"
                        font.pixelSize: App.secondaryTextSize
                        focusPolicy: Qt.NoFocus
                        ToolTip.visible: hovered
                        ToolTip.text: "Hand this file to the desktop's default application"
                        onClicked: App.openExternally(controller.currentUri)
                    }
                }
            }

            // The viewer, and the drawer beside it. A long vertical list belongs
            // down the side rather than across the top, where every row of it
            // cost the picture room whether or not anybody was reading it.
            SplitView {
                id: split
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal

                handle: Rectangle {
                    implicitWidth: 5
                    color: SplitHandle.pressed ? Material.accent
                                               : (SplitHandle.hovered ? App.colour.border : App.colour.hover)
                    // Written when the divider is let go rather than as it moves:
                    // a preference is a file on disk.
                    onPressedChanged: if (!SplitHandle.pressed && controller && drawer.visible)
                                          controller.setDetailsWidth(Math.round(drawer.width))
                    property bool pressed: SplitHandle.pressed
                }

                // One viewer at a time. Reloaded from scratch on every file so a
                // heavy one cannot linger.
                Loader {
                    id: viewerLoader
                    SplitView.fillWidth: true
                    // The picture keeps a usable width at every window size, so
                    // turning the drawer on narrows it rather than replacing it.
                    SplitView.minimumWidth: 260
                    source: controller ? controller.viewSource : ""
                    onLoaded: {
                        if (!item)
                            return
                        item.controller = controller ? controller.viewer : null
                        // A viewer that renders the facts itself needs the tab
                        // that owns them; the rest have no such property.
                        if (item.hasOwnProperty("tab"))
                            item.tab = controller
                    }
                }

                DetailsList {
                    id: drawer
                    objectName: "detailsPanel"
                    // Not for the viewer whose content the facts already are:
                    // the information viewer renders this same list in its body,
                    // and two copies of it would be one too many.
                    visible: controller && controller.detailsOpen
                             && !(viewerLoader.item && viewerLoader.item.showsDetailsItself === true)
                    SplitView.preferredWidth: controller ? controller.detailsWidth : 320
                    SplitView.minimumWidth: 200
                    SplitView.maximumWidth: Math.max(240, split.width / 2)

                    facts: controller ? controller.details : []
                    busy: controller ? controller.detailsLoading : false
                    onCopyAll: function() { if (controller) controller.copyDetails() }
                }
            }

            Connections {
                target: controller
                function onCurrentChanged() {
                    if (!viewerLoader.item)
                        return
                    // Not when the markup is about to be replaced as well. The
                    // item still loaded belongs to the viewer being stepped away
                    // from, and pushing the new controller into it binds
                    // properties that markup does not have -- a video view asked
                    // for `source` on the list of facts. The Loader is about to
                    // build the right item, and onLoaded does it there.
                    if (viewerLoader.source !== controller.viewSource)
                        return
                    viewerLoader.item.controller = controller.viewer
                    if (viewerLoader.item.hasOwnProperty("tab"))
                        viewerLoader.item.tab = controller
                }
            }

            // While the head of the file is being read there is no viewer yet and
            // none has been ruled out, so neither of these would be true.
            Label {
                Layout.fillWidth: true
                Layout.margins: 12
                visible: controller && controller.viewSource.toString().length === 0
                         && !controller.identifying
                wrapMode: Text.Wrap
                color: App.colour.textMuted
                text: controller && controller.currentUri.length > 0
                      ? "Nothing installed can show this file."
                      : "Nothing open. Press F3 on a file in the browser."
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.bottomMargin: 4
                text: controller ? controller.folderPath : ""
                elide: Text.ElideLeft
                color: App.colour.textFaint
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
