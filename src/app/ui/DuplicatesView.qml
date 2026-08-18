import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Finding duplicates, and deciding what to do about them.
//
// Choosing what to keep is the hard half, and this never picks for you. It
// offers the choices people actually make — keep the newest, the oldest, the one
// nearest the top of the tree — and says what each would free before anything is
// deleted.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"

    // Which of the four states the tab is in. Written once here rather than as
    // four conditions repeated down the file, because the one thing they have to
    // be is mutually exclusive: two of them true at once is how the tab ended up
    // with a strip of content above a void.
    readonly property bool hasRoots: controller && controller.roots.length > 0
    readonly property bool scanning: controller !== null && controller.scanning
    readonly property bool showingResults: controller !== null && controller.groupCount > 0
    readonly property bool foundNothing: controller !== null && !scanning && controller.hasRun
                                         && controller.groupCount === 0

    function focusActivePane() { body.forceActiveFocus() }

    // A root as somebody reads it. The scheme stays on anything that is not the
    // local disk, because on two drives it is the only thing telling the two
    // apart -- and it comes off file:// paths, where it is noise.
    function readableRoot(uri) {
        return uri.startsWith("file://") ? uri.substring(7) : uri
    }

    // What the chosen strategy is called, for a sentence about it rather than a
    // caption underneath one.
    function strategyLabel() {
        if (!controller)
            return ""
        const all = controller.strategies
        for (let i = 0; i < all.length; ++i) {
            if (all[i].id === controller.strategyId)
                return all[i].label
        }
        return ""
    }

    FocusScope {
        id: body
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: "Duplicates"
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    objectName: "duplicateSummary"
                    text: controller ? controller.summary : ""
                    color: view.mutedColor
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.scanning : false
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                Button {
                    objectName: "scanButton"
                    text: controller && controller.scanning ? "Stop" : "Scan"
                    highlighted: !controller || !controller.scanning
                    enabled: controller && controller.roots.length > 0
                    onClicked: controller.scanning ? controller.cancel() : controller.scan()
                }
            }

            // ---- what and how ----------------------------------------------

            Rectangle {
                Layout.fillWidth: true
                radius: 6
                color: view.panelColor
                border.width: 1
                border.color: view.lineColor
                implicitHeight: options.implicitHeight + 22

                ColumnLayout {
                    id: options
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 6

                    // What is being searched. One row each rather than a
                    // `\n`-joined label elided in the middle: these are the answer
                    // to "what am I even looking at", there are rarely more than a
                    // handful, and eliding the join hides which drive each is on.
                    Label {
                        objectName: "duplicateNoRoots"
                        Layout.fillWidth: true
                        visible: !view.hasRoots
                        text: "Open this from a folder to search it."
                        color: view.mutedColor
                        font.pixelSize: 11
                    }

                    Repeater {
                        model: view.hasRoots ? controller.roots : []
                        delegate: RowLayout {
                            required property string modelData

                            objectName: "duplicateRoot"
                            Layout.fillWidth: true
                            spacing: 6

                            Label {
                                text: "▸"
                                color: "#6f7788"
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                text: view.readableRoot(modelData)
                                color: "#c9d1d9"
                                font.pixelSize: 11
                                font.family: App.monospaceFont
                                // From the left: what tells two folders apart is
                                // the end of the path, not the start.
                                elide: Text.ElideLeft
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            text: "Match by"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        ComboBox {
                            objectName: "strategyPicker"
                            implicitContentWidthPolicy: ComboBox.WidestText
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "id"
                            model: controller ? controller.strategies : []
                            currentIndex: {
                                if (!controller)
                                    return 0
                                const all = controller.strategies
                                for (let i = 0; i < all.length; ++i) {
                                    if (all[i].id === controller.strategyId)
                                        return i
                                }
                                return 0
                            }
                            onActivated: controller.strategyId = currentValue
                        }

                        Label {
                            text: "Ignore below"
                            color: view.mutedColor
                            font.pixelSize: 12
                        }
                        ComboBox {
                            objectName: "minimumSizePicker"
                            implicitContentWidthPolicy: ComboBox.WidestText
                            font.pixelSize: 12
                            textRole: "label"
                            valueRole: "bytes"
                            model: [
                                { bytes: 1, label: "anything" },
                                { bytes: 1024, label: "1 kB" },
                                { bytes: 102400, label: "100 kB" },
                                { bytes: 1048576, label: "1 MB" },
                                { bytes: 10485760, label: "10 MB" }
                            ]
                            currentIndex: {
                                if (!controller)
                                    return 1
                                for (let i = 0; i < model.length; ++i) {
                                    if (model[i].bytes === controller.minimumSize)
                                        return i
                                }
                                return 1
                            }
                            onActivated: controller.minimumSize = currentValue
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // What this choice costs, not only what it matches -- the part
                    // somebody needs before starting a scan on a NAS. Kept here as
                    // well as in the empty state, because the picker is right above
                    // it and this is where somebody changing it is looking.
                    Label {
                        objectName: "duplicateStrategyNote"
                        Layout.fillWidth: true
                        text: controller ? controller.strategyDescription : ""
                        color: "#6f7788"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                }
            }

            // ---- what to keep ----------------------------------------------

            RowLayout {
                Layout.fillWidth: true
                visible: controller && controller.groupCount > 0
                spacing: 8

                Label {
                    text: "Keep"
                    color: view.mutedColor
                    font.pixelSize: 12
                }
                Button {
                    text: "Newest"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepNewest()
                }
                Button {
                    text: "Oldest"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepOldest()
                }
                Button {
                    text: "Nearest the top"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.keepShortestPath()
                    ToolTip.visible: hovered
                    ToolTip.text: "The copy with the shortest path is usually the original"
                }
                Button {
                    text: "Nothing"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.clearSelection()
                }

                Item { Layout.fillWidth: true }

                Label {
                    objectName: "duplicateSelection"
                    visible: controller && controller.selectedCount > 0
                    text: controller
                          ? controller.selectedCount + " ticked · " + controller.selectedSizeText
                          : ""
                    color: "#d9a441"
                    font.pixelSize: 12
                }
                Button {
                    objectName: "deleteDuplicatesButton"
                    text: "Delete ticked"
                    enabled: controller && controller.selectedCount > 0
                    onClicked: confirmDelete.open()
                }
            }

            // ---- the body ---------------------------------------------------
            //
            // One item that always claims the height, with a panel inside it for
            // each state. An invisible item is dropped from a ColumnLayout
            // altogether rather than reserving its space, so binding
            // `Layout.fillHeight` straight onto the group list -- which is what
            // this used to be -- left the whole tab collapsed upward into a strip
            // of content above a void whenever there was nothing to show. Which
            // was every time somebody opened it.

            Item {
                objectName: "duplicateBody"
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Nothing scanned yet. The state the tab is in the first time
                // anybody sees it, and so the one that has to say what this is
                // for -- what will be searched, what the strategy matches and
                // costs, and that the scan is the next step.
                ColumnLayout {
                    objectName: "duplicateEmptyState"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 520)
                    visible: !view.scanning && !view.showingResults && !view.foundNothing
                    spacing: 10

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "⧉"
                        font.pixelSize: 44
                        color: "#3a4152"
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: view.hasRoots ? "Nothing scanned yet" : "Nothing to search"
                        font.pixelSize: 16
                        font.bold: true
                        color: "#c9d1d9"
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        color: view.mutedColor
                        text: {
                            if (!view.hasRoots)
                                return "Open this tab from a folder and it will search that folder."
                            const count = controller.roots.length
                            const where = count === 1
                                  ? view.readableRoot(controller.roots[0])
                                  : count + " folders"
                            return "Scanning will walk " + where + ", matching by " +
                                   view.strategyLabel().toLowerCase() + "."
                        }
                    }
                    // The cost, given the weight it deserves. It was 11px grey at
                    // the bottom of a panel, which is where somebody about to
                    // start a scan on a NAS was least likely to read it.
                    Label {
                        objectName: "duplicateEmptyStateCost"
                        Layout.fillWidth: true
                        visible: view.hasRoots && text.length > 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        color: "#6f7788"
                        text: controller ? controller.strategyDescription : ""
                    }
                    Button {
                        objectName: "duplicateEmptyStateScan"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 4
                        visible: view.hasRoots
                        text: "Scan"
                        highlighted: true
                        onClicked: controller.scan()
                    }
                }

                // Scanning. The space belongs to progress -- there was a
                // BusyIndicator in the header and nothing else, which on a large
                // tree is a spinner and a silence.
                ColumnLayout {
                    objectName: "duplicateScanningState"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 520)
                    visible: view.scanning && !view.showingResults
                    spacing: 10

                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: parent.visible
                        implicitWidth: 44
                        implicitHeight: 44
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Searching"
                        font.pixelSize: 16
                        font.bold: true
                        color: "#c9d1d9"
                    }
                    Label {
                        objectName: "duplicateScanningDetail"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        color: view.mutedColor
                        // What the scan has to say for itself. Thin today -- the
                        // stage it is on and how much is left is MOLE-70's job,
                        // and this is the place it will land.
                        text: controller ? controller.summary : ""
                    }
                    Button {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 4
                        text: "Stop"
                        flat: true
                        onClicked: controller.cancel()
                    }
                }

                // Nothing matched. Already the best-written part of this view, so
                // the wording is untouched -- only where it sits has changed.
                ColumnLayout {
                    objectName: "duplicateNoMatchState"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 520)
                    visible: view.foundNothing
                    spacing: 10

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "✓"
                        font.pixelSize: 40
                        color: "#3a4152"
                    }
                    Label {
                        objectName: "duplicateNoMatchText"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        color: view.mutedColor
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        text: "Nothing matched. A different strategy may still find something — " +
                              "'Identical contents' proves files are the same, while 'Same name' " +
                              "finds copies that were edited apart."
                    }
                }

            // ---- the groups -------------------------------------------------

            ListView {
                objectName: "duplicateGroupList"
                anchors.fill: parent
                visible: view.showingResults
                clip: true
                spacing: 8
                model: controller ? controller.groups : []

                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: groupBody.implicitHeight + 18
                    radius: 6
                    color: view.panelColor
                    border.width: 1
                    border.color: view.lineColor

                    ColumnLayout {
                        id: groupBody
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.count + " copies · " + modelData.sizeText + " each"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: modelData.reclaimableText + " could be freed"
                                color: "#d9a441"
                                font.pixelSize: 11
                            }
                        }

                        Repeater {
                            model: modelData.files
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                CheckBox {
                                    checked: modelData.selected
                                    onToggled: controller.toggle(modelData.uri)
                                }
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    font.family: App.monospaceFont
                                    Layout.preferredWidth: 220
                                    elide: Text.ElideMiddle
                                    color: modelData.selected ? "#d9a441" : "#d5dbe6"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.location
                                    color: view.mutedColor
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.modifiedText
                                    color: view.mutedColor
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }
            }
            }
        }
    }

    Dialog {
        id: confirmDelete
        objectName: "confirmDeleteDuplicates"
        anchors.centerIn: parent
        modal: true
        title: "Delete these files?"
        focus: true
        footer: ConfirmButtons {
            acceptText: "Delete"
            rejectText: "Keep"
            destructive: true
        }
        width: 520

        // Snapshotted when the question is asked, so a tick landing behind the
        // dialog cannot change what pressing Ok means.
        property var doomed: []

        onAboutToShow: doomed = controller ? controller.selectedDetails() : []
        onAccepted: controller.deleteSelected()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: App.textSize
                text: controller
                      ? controller.selectedCount + " files, " + controller.selectedSizeText + "."
                      : ""
            }
            // By location, not by name: in a duplicate group every name is the
            // same, so a list of names would be no help at all.
            TargetList {
                objectName: "duplicateDeleteList"
                Layout.fillWidth: true
                model: confirmDelete.doomed
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "This cannot be undone, and the copies you did not tick are left alone."
                color: "#d9a441"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
