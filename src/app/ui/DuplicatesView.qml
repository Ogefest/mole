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
    //
    // Asked rather than sliced. The hardcoded 7 turned file:///C:/x into /C:/x,
    // and would have gone on being subtly wrong whatever spelling a drive letter
    // ended up with.
    function readableRoot(uri) {
        return App.pathTextFor(uri)
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
                    color: App.colour.textMuted
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.scanning : false
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                ActionButton {
                    objectName: "scanButton"
                    text: controller && controller.scanning ? "Stop" : "Scan"
                    filled: !controller || !controller.scanning
                    enabled: controller && controller.roots.length > 0
                    onClicked: controller.scanning ? controller.cancel() : controller.scan()
                }
            }

            // ---- what and how ----------------------------------------------

            Rectangle {
                Layout.fillWidth: true
                radius: 6
                color: App.colour.panel
                border.width: 1
                border.color: App.colour.border
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
                        color: App.colour.textMuted
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
                                color: App.colour.textFaint
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                text: view.readableRoot(modelData)
                                color: App.colour.textSecondary
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
                            color: App.colour.textMuted
                            font.pixelSize: 12
                        }
                        Picker {
                            objectName: "strategyPicker"
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
                            color: App.colour.textMuted
                            font.pixelSize: 12
                        }
                        Picker {
                            objectName: "minimumSizePicker"
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
                        color: App.colour.textFaint
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                }
            }

            // ---- what the scan is doing ------------------------------------
            //
            // Only once there is something on screen to be beside. Before that the
            // progress has the whole body and there is nothing to compete with.

            RowLayout {
                objectName: "duplicateProgressStrip"
                Layout.fillWidth: true
                visible: view.scanning && view.showingResults
                spacing: 8

                BusyIndicator {
                    running: parent.visible
                    implicitWidth: 16
                    implicitHeight: 16
                }
                Label {
                    objectName: "duplicateProgressText"
                    Layout.fillWidth: true
                    text: controller ? controller.progressText : ""
                    color: App.colour.textMuted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                // The results below are what has been confirmed so far, and saying
                // so is the difference between a list that is still filling and one
                // that is the answer.
                Label {
                    text: "still searching"
                    color: App.colour.warn
                    font.pixelSize: 12
                }
                Button {
                    text: "Stop"
                    flat: true
                    font.pixelSize: 12
                    onClicked: controller.cancel()
                }
            }

            // ---- what to keep ----------------------------------------------
            //
            // Choosing what to keep is the hard half of deduplication, and it used
            // to be four flat buttons wedged between the options and the results
            // with the least visual weight of anything on the screen. It is a
            // panel now, with the same weight as the options above it, because it
            // is the control that decides what gets deleted.
            //
            // Two verbs, deliberately. The *rule* is stated as keep, because that
            // is how the decision is made -- "keep the newest". The *ticks* are
            // stated as remove, because that is what they do. Saying only one of
            // them left a screen where pressing "Newest" under a heading reading
            // Keep put a tick against every file except the newest, and nothing
            // said which of the two readings a tick meant. See ADR-0044.

            Rectangle {
                objectName: "duplicateKeepPanel"
                Layout.fillWidth: true
                visible: controller && controller.groupCount > 0
                radius: 6
                color: App.colour.panel
                border.width: 1
                border.color: App.colour.border
                implicitHeight: keeping.implicitHeight + 22

                ColumnLayout {
                    id: keeping
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: "Keep"
                            color: App.colour.textSecondary
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Button {
                            objectName: "keepNewestButton"
                            text: "Newest"
                            font.pixelSize: 12
                            onClicked: controller.keepNewest()
                        }
                        Button {
                            text: "Oldest"
                            font.pixelSize: 12
                            onClicked: controller.keepOldest()
                        }
                        Button {
                            text: "Nearest the top"
                            font.pixelSize: 12
                            onClicked: controller.keepShortestPath()
                            ToolTip.visible: hovered
                            ToolTip.text: "The copy with the shortest path is usually the original"
                        }
                        Button {
                            objectName: "keepEverythingButton"
                            text: "Everything"
                            flat: true
                            font.pixelSize: 12
                            onClicked: controller.clearSelection()
                            ToolTip.visible: hovered
                            ToolTip.text: "Unticks the lot, so nothing would be removed"
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            objectName: "makeSetFromDuplicatesButton"
                            text: "Make a set"
                            flat: true
                            enabled: controller && controller.selectedCount > 0
                            ToolTip.visible: hovered
                            ToolTip.text: "The ticked copies become a file set, which every " +
                                          "operation in Mole takes"
                            // openPlace() rather than openFeatureTab(): the sets
                            // are a standing tool that exists once (ADR-0032), and
                            // openFeatureTab() always builds a new tab -- so this
                            // left two Sets tabs when it was pressed twice.
                            // openPlace() shows the one tab *and* points it at the
                            // set just built, which is the half that makes reuse
                            // right: a reused tab still showing whichever set was
                            // current before would file the new one out of sight.
                            // See MOLE-254.
                            onClicked: {
                                const id = controller.buildSetFromTicked("")
                                if (id.length > 0)
                                    App.openPlace("set", id)
                            }
                        }
                        Button {
                            objectName: "deleteDuplicatesButton"
                            text: "Delete ticked"
                            enabled: controller && controller.selectedCount > 0
                            onClicked: confirmDelete.open()
                        }
                    }

                    // What the rule just did, across every group at once. Without
                    // it a rule was applied silently and the only evidence was a
                    // count in the corner, which says nothing about whether the
                    // rule was the right one.
                    Label {
                        objectName: "duplicateRuleText"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: controller && controller.selectedCount > 0 ? App.colour.warn : App.colour.textFaint
                        text: {
                            if (!controller)
                                return ""
                            if (controller.selectedCount === 0)
                                return "Nothing is ticked, so nothing would be removed."
                            const rule = controller.ruleText.length > 0
                                  ? controller.ruleText
                                  : "Chosen by hand"
                            return rule + " — " + controller.selectedCount + " of " +
                                   controller.copyCount + " copies ticked for removal, " +
                                   controller.selectedSizeText
                        }
                    }
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
                        color: App.colour.border
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: view.hasRoots ? "Nothing scanned yet" : "Nothing to search"
                        font.pixelSize: 16
                        font.bold: true
                        color: App.colour.textSecondary
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        color: App.colour.textMuted
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
                        color: App.colour.textFaint
                        text: controller ? controller.strategyDescription : ""
                    }
                    ActionButton {
                        objectName: "duplicateEmptyStateScan"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 4
                        visible: view.hasRoots
                        text: "Scan"
                        onClicked: controller.scan()
                    }
                }

                // Scanning, with nothing found yet. The space belongs to progress --
                // there was a BusyIndicator in the header and nothing else, which
                // on a large tree is a spinner and a silence. Once the first group
                // is confirmed the results take the space and progress carries on
                // in the strip below the options.
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
                        color: App.colour.textSecondary
                    }
                    Label {
                        objectName: "duplicateScanningDetail"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        color: App.colour.textMuted
                        // Which stage is running and over how much. "whole file:
                        // 87 of 412 files" is something somebody can decide to
                        // wait for; a spinner is not.
                        text: controller ? controller.progressText : ""
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
                        text: controller && controller.wasCancelled ? "⏹" : "✓"
                        font.pixelSize: 40
                        color: App.colour.border
                    }
                    // A scan that was stopped has not searched the tree, so the
                    // advice below would be answering a question nobody asked --
                    // "nothing matched" is a claim only a scan that ran to the end
                    // is entitled to make.
                    Label {
                        objectName: "duplicateNoMatchText"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        color: App.colour.textMuted
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.secondaryTextSize
                        text: controller && controller.wasCancelled
                              ? "Stopped before anything was found. The rest of the tree has not " +
                                "been searched — scanning again starts from the beginning."
                              : "Nothing matched. A different strategy may still find something — " +
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
                // A model, not a list: a confirmed group is one row inserted,
                // so the delegates already built are left alone and the scroll
                // position with them. See MOLE-210 and DuplicateGroupModel.
                model: controller ? controller.groups : null

                delegate: Rectangle {
                    required property int copies
                    required property string sizeText
                    required property string reclaimableText
                    required property var files

                    // Whether anything in this group is ticked. Until something is,
                    // every copy is equally kept and marking them all "keeping"
                    // would be noise on a screen where nothing has been decided.
                    readonly property bool decided: {
                        for (let i = 0; i < files.length; ++i) {
                            if (files[i].selected)
                                return true
                        }
                        return false
                    }

                    width: ListView.view.width
                    implicitHeight: groupBody.implicitHeight + 18
                    radius: 6
                    color: App.colour.panel
                    border.width: 1
                    border.color: App.colour.border

                    ColumnLayout {
                        id: groupBody
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: copies + " copies · " + sizeText + " each"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Item { Layout.fillWidth: true }
                            // Whether this group has been decided at all, so a rule
                            // applied to fifty of them can be checked by scrolling
                            // rather than by counting ticks.
                            Label {
                                objectName: "duplicateGroupUndecided"
                                visible: !decided
                                text: "not decided"
                                color: App.colour.textFaint
                                font.pixelSize: 11
                            }
                            Label {
                                text: reclaimableText + " could be freed"
                                color: App.colour.warn
                                font.pixelSize: 11
                            }
                        }

                        Repeater {
                            model: files
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                // A tick removes. The column is headed that way in
                                // the panel above, and the row says the other half:
                                // in a decided group the unticked copy is the one
                                // being kept, and it says so in a word.
                                CheckBox {
                                    checked: modelData.selected
                                    onToggled: controller.toggle(modelData.uri)
                                    ToolTip.visible: hovered
                                    ToolTip.text: modelData.selected
                                                  ? "Ticked: this copy would be removed"
                                                  : "Not ticked: this copy stays"
                                }
                                Label {
                                    objectName: "duplicateKeepMark"
                                    Layout.preferredWidth: 52
                                    visible: decided
                                    text: modelData.selected ? "remove" : "keeping"
                                    horizontalAlignment: Text.AlignRight
                                    color: modelData.selected ? App.colour.bad : App.colour.ok
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    font.family: App.monospaceFont
                                    Layout.preferredWidth: 200
                                    elide: Text.ElideMiddle
                                    color: modelData.selected ? App.colour.warn : App.colour.textSecondary
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.location
                                    color: App.colour.textMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.modifiedText
                                    color: App.colour.textMuted
                                    font.pixelSize: 11
                                }
                                // The per-group override, in one click on the row
                                // that is already there. A rule right for
                                // forty-nine groups and wrong for one should not
                                // have to be abandoned for the whole scan, and
                                // four rule buttons per group would be two hundred
                                // controls saying what this says.
                                Button {
                                    objectName: "keepOnlyThisButton"
                                    text: "Keep this one"
                                    flat: true
                                    font.pixelSize: 11
                                    visible: !decided || modelData.selected
                                    onClicked: controller.keepOnly(modelData.uri)
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
        // A dialog sits on the panel ground, said here rather than inherited:
        // the window no longer hands one down. See ADR-0074.
        Material.background: App.colour.panel
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

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
        // The rows that were shown, handed back -- see MOLE-339. A scan may still
        // be confirming groups behind the dialog, and reading the ticks again
        // here deleted what was ticked at that instant rather than what was
        // named above.
        onAccepted: controller.deleteSelected(doomed.map(function(row) { return row.uri }))

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: App.textSize
                // From the snapshot, not from the live count: the headline used
                // to be a binding, so a tick landing behind the dialog changed
                // the number while the names below it stayed as they were --
                // which is the one thing the snapshot exists to prevent.
                text: confirmDelete.doomed.length
                      + (confirmDelete.doomed.length === 1 ? " file, " : " files, ")
                      + (controller ? controller.selectedSizeText : "") + "."
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
                color: App.colour.warn
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
