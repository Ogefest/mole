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
                Material.background: "#1b2029"

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
                        color: "#8b93a7"
                        font.pixelSize: App.secondaryTextSize
                    }

                    ToolSeparator {}

                    Label {
                        text: controller ? controller.viewerName : ""
                        color: Material.accent
                        font.pixelSize: App.smallTextSize
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
                                color: "#8b93a7"
                                font.pixelSize: App.smallTextSize
                            }
                            ComboBox {
                                objectName: "viewerOption_" + modelData.key
                                implicitContentWidthPolicy: ComboBox.WidestText
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
                                               : (SplitHandle.hovered ? "#39445a" : "#232a35")
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
                color: "#8b93a7"
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
                color: "#6f7788"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
