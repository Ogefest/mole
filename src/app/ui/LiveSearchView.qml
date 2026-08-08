import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tab body for the live search feature.
Item {
    id: view

    property var controller: null

    // The tab is opened with a key, so the box it exists for has the keyboard from
    // the start -- reaching for the mouse to click into a search field is exactly
    // what Ctrl+F is supposed to save.
    function focusActivePane() { queryField.forceActiveFocus() }
    Component.onCompleted: Qt.callLater(queryField.forceActiveFocus)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 8
            rowSpacing: 6

            Label { text: "Search in"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            TextField {
                Layout.fillWidth: true
                Layout.columnSpan: 3
                text: controller ? controller.rootUri : ""
                selectByMouse: true
                font.pixelSize: App.secondaryTextSize
                onEditingFinished: if (controller) controller.rootUri = text
            }

            Label { text: "Name contains"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            TextField {
                id: queryField
                objectName: "searchQueryField"
                Layout.fillWidth: true
                text: controller ? controller.queryText : ""
                selectByMouse: true
                font.pixelSize: App.secondaryTextSize
                onTextChanged: if (controller) controller.queryText = text
                onAccepted: if (controller) controller.start()
                // Down out of the box and into the results: once a search has
                // answered, the answers are where the keyboard should be.
                Keys.onDownPressed: resultList.takeFocus()
            }

            Label { text: "Extension"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            TextField {
                Layout.preferredWidth: 120
                placeholderText: "pdf"
                text: controller ? controller.extension : ""
                font.pixelSize: App.secondaryTextSize
                onTextChanged: if (controller) controller.extension = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: controller && controller.running ? "Stop" : "Search"
                highlighted: true
                onClicked: {
                    if (!controller)
                        return
                    controller.running ? controller.stop() : controller.start()
                }
            }

            CheckBox {
                text: "Case sensitive"
                font.pixelSize: App.secondaryTextSize
                checked: controller ? controller.caseSensitive : false
                onToggled: if (controller) controller.caseSensitive = checked
            }

            BusyIndicator {
                running: controller ? controller.running : false
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }

            Label {
                Layout.fillWidth: true
                text: controller ? controller.statusText : ""
                color: "#8b93a7"
                elide: Text.ElideRight
                font.pixelSize: App.secondaryTextSize
            }
        }

        // What to do with the results once there are some: narrow them without
        // walking the disk again, or take them somewhere the work continues.
        RowLayout {
            Layout.fillWidth: true
            visible: controller && controller.results && controller.results.totalCount > 0
            spacing: 8

            Label {
                text: "Narrow"
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
            }
            TextField {
                objectName: "narrowResultsField"
                Layout.preferredWidth: 220
                placeholderText: "Filter these results…"
                font.pixelSize: App.secondaryTextSize
                // Straight onto the model that already holds the matches: no walk,
                // no query, just less of what is there.
                onTextEdited: if (controller && controller.results) controller.results.filterText = text
                Keys.onEscapePressed: {
                    text = ""
                    if (controller && controller.results)
                        controller.results.filterText = ""
                }
            }
            Label {
                objectName: "narrowCount"
                text: controller && controller.results
                      ? (controller.results.count === controller.results.totalCount
                            ? controller.results.count + " results"
                            : controller.results.count + " of " + controller.results.totalCount)
                      : ""
                color: "#6f7788"
                font.pixelSize: App.smallTextSize
            }

            Item { Layout.fillWidth: true }

        }

        // Folded away until wanted, so the common case stays one field and one key.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ToolButton {
                objectName: "advancedToggle"
                text: (advanced.visible ? "▾  " : "▸  ") + "More"
                font.pixelSize: App.secondaryTextSize
                focusPolicy: Qt.NoFocus
                onClicked: advanced.visible = !advanced.visible
            }
            Item { Layout.fillWidth: true }
        }

        GridLayout {
            id: advanced
            objectName: "advancedCriteria"
            Layout.fillWidth: true
            visible: false
            columns: 5
            columnSpacing: 8
            rowSpacing: 6

            Label { text: "Size from"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            TextField {
                id: minSizeField
                objectName: "minSizeField"
                Layout.preferredWidth: 110
                placeholderText: "10M"
                font.pixelSize: App.secondaryTextSize
                onTextEdited: if (controller) controller.setSizeRange(minSizeField.text, maxSizeField.text)
            }
            Label { text: "to"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            TextField {
                id: maxSizeField
                objectName: "maxSizeField"
                Layout.preferredWidth: 110
                placeholderText: "2G"
                font.pixelSize: App.secondaryTextSize
                onTextEdited: if (controller) controller.setSizeRange(minSizeField.text, maxSizeField.text)
            }
            Item { Layout.fillWidth: true }

            // The index answers instantly and might be out of date, so the toggle
            // sits with the criteria and says how old it is. See ADR-0005.
            CheckBox {
                objectName: "useIndexToggle"
                Layout.columnSpan: 2
                text: "Use the index"
                enabled: controller ? controller.indexCoversRoot : false
                checked: controller ? controller.useIndex : true
                font.pixelSize: App.secondaryTextSize
                onToggled: if (controller) controller.useIndex = checked
            }
            Label {
                Layout.columnSpan: 3
                Layout.fillWidth: true
                text: controller && controller.indexNote.length > 0
                      ? controller.indexNote
                      : "This folder is not indexed, so searching walks it."
                color: "#6f7788"
                font.pixelSize: App.smallTextSize
                elide: Text.ElideRight
            }
        }

        Label {
            Layout.fillWidth: true
            visible: controller ? controller.truncated : false
            text: "Result limit reached — this list is incomplete."
            color: Material.color(Material.Amber)
            font.pixelSize: App.secondaryTextSize
        }

        // A walk over a large disk takes long enough that silence reads as nothing
        // having happened. Shown only while there is nothing to show yet: once rows
        // start arriving they are the better answer to "is this working".
        Item {
            objectName: "searchWaitingView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controller && controller.running === true
                     && (controller.results ? controller.results.count === 0 : true)

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 420)
                spacing: 10

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: parent.parent.visible
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Searching…"
                    font.pixelSize: App.textSize
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: controller && controller.statusText.length > 0
                          ? controller.statusText
                          : "Results appear as they are found."
                    color: "#6f7788"
                    font.pixelSize: App.smallTextSize
                }
            }
        }

        SearchResultList {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            canBuildSet: true
            onBuildSetRequested: {
                const id = controller.buildSetFromResults("")
                if (id.length > 0)
                    App.openFeatureTab("core.filesets")
            }
            visible: !(controller && controller.running === true
                       && (controller.results ? controller.results.count === 0 : true))
            resultsModel: controller ? controller.results : null
        }
    }
}
