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

            // One viewer at a time. Reloaded from scratch on every file so a
            // heavy one cannot linger.
            Loader {
                id: viewerLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                source: controller ? controller.viewSource : ""
                onLoaded: if (item) item.controller = controller ? controller.viewer : null
            }

            Connections {
                target: controller
                function onCurrentChanged() {
                    if (viewerLoader.item)
                        viewerLoader.item.controller = controller.viewer
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
