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

            // Where to search is a field, not a tab. Searching everything ever
            // scanned used to be a separate window with its own form and its own
            // idea of what a search was; it is a scope, and the difference
            // between the two was only ever which engine could answer.
            Label { text: "Search in"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
            ComboBox {
                objectName: "searchScope"
                Layout.preferredWidth: 200
                model: ["This folder", "Everywhere indexed"]
                currentIndex: controller && controller.everywhere ? 1 : 0
                font.pixelSize: App.secondaryTextSize
                onActivated: if (controller) controller.everywhere = (currentIndex === 1)
            }
            TextField {
                objectName: "searchRootField"
                Layout.fillWidth: true
                Layout.columnSpan: 2
                visible: !(controller && controller.everywhere)
                text: controller ? controller.rootUri : ""
                selectByMouse: true
                font.pixelSize: App.secondaryTextSize
                onEditingFinished: if (controller) controller.rootUri = text
            }
            // In its place when the scope is everywhere: which of the scanned
            // volumes, and how much each holds. The retired tab's one control.
            ComboBox {
                objectName: "searchVolume"
                Layout.fillWidth: true
                Layout.columnSpan: 2
                visible: controller && controller.everywhere === true
                model: controller ? controller.volumeLabels : []
                currentIndex: controller ? controller.volumeIndex : 0
                font.pixelSize: App.secondaryTextSize
                onActivated: if (controller) controller.volumeIndex = currentIndex
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

            // Indexing a folder is what makes "everywhere indexed" mean
            // anything, so the way to do it belongs beside the search rather
            // than in a tab somebody has to know about.
            Button {
                objectName: "scanFolderButton"
                text: "Scan a folder…"
                flat: true
                font.pixelSize: App.secondaryTextSize
                onClicked: scanDialog.open()
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
            // sits with the criteria and says how old it is. See ADR-0005. It is
            // about a folder: searching everywhere indexed is the index by
            // definition, so there is nothing there to turn off.
            CheckBox {
                objectName: "useIndexToggle"
                Layout.columnSpan: 2
                visible: !(controller && controller.everywhere)
                text: "Use the index"
                enabled: controller ? controller.indexCoversRoot : false
                checked: controller ? controller.useIndex : true
                font.pixelSize: App.secondaryTextSize
                onToggled: if (controller) controller.useIndex = checked
            }
            Label {
                objectName: "indexNote"
                Layout.columnSpan: controller && controller.everywhere ? 5 : 3
                Layout.fillWidth: true
                text: controller && controller.everywhere
                      ? "Answered from what the last scan of each volume recorded."
                      : (controller && controller.indexNote.length > 0
                            ? controller.indexNote
                            : "This folder is not indexed, so searching walks it.")
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

    Dialog {
        id: scanDialog
        objectName: "scanDialog"
        // Without this the popup never becomes a focus scope, so nothing inside it
        // can hold the keyboard and the footer's focus quietly does nothing.
        focus: true
        title: "Index a folder"
        modal: true
        anchors.centerIn: parent
        width: 520

        footer: ConfirmButtons {
            acceptText: "Index"
            // Nothing to index without a path, and a button that acts on nothing
            // is worse than one that says it cannot.
            acceptEnabled: scanPath.text.trim().length > 0
            // Typed into first, so the field wins over the button.
            keyboardOn: "none"
        }

        onOpened: scanPath.forceActiveFocus()

        onAccepted: {
            if (controller && scanPath.text.length > 0) {
                var uri = scanPath.text.trim()
                if (uri.indexOf("://") < 0)
                    uri = "file://" + uri
                controller.scanDirectory(uri, scanLabel.text)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Walks the tree once in the background and records it, so searching it later never touches the disk."
                color: "#8b93a7"
                font.pixelSize: App.secondaryTextSize
            }

            TextField {
                id: scanPath
                objectName: "scanPath"
                Layout.fillWidth: true
                placeholderText: "/home/you/big-archive"
                selectByMouse: true
                // The keyboard starts here, so Return has to answer from here.
                onAccepted: if (text.trim().length > 0) scanDialog.accept()
            }

            TextField {
                id: scanLabel
                Layout.fillWidth: true
                placeholderText: "Label (optional)"
                selectByMouse: true
                onAccepted: if (scanPath.text.trim().length > 0) scanDialog.accept()
            }
        }
    }
}
