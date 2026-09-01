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
    //
    // That box is the line. It used to be the Name contains field, which had the
    // id while the line had only an objectName -- and once the name field moved
    // behind More, focusing it would have put the keyboard inside a hidden panel,
    // which is to say nowhere. See ADR-0067.
    function focusActivePane() { queryLine.forceActiveFocus() }

    /// F3, asked for by name by the window. A search tab has no pane for the
    /// window to resolve the key through; the results list is where its cursor
    /// is. See MOLE-204.
    function previewCurrentRow() { resultList.previewCurrentRow() }
    Component.onCompleted: Qt.callLater(queryLine.forceActiveFocus)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // The line and the form are one query seen twice. Typing here moves the
        // fields; changing a field rewrites this. Neither is the master: the
        // line teaches the form's vocabulary to somebody who started with the
        // mouse, and the form explains the line to somebody who started typing.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            TextField {
                id: queryLine
                objectName: "queryLineField"
                Layout.fillWidth: true
                placeholderText: "report ext:pdf size>10M modified:<30d"
                font.family: App.monospaceFont
                font.pixelSize: App.secondaryTextSize
                text: controller ? controller.queryLine : ""
                selectByMouse: true
                onTextEdited: if (controller) controller.queryLine = text
                onAccepted: if (controller) controller.start()
                Keys.onDownPressed: resultList.takeFocus()
            }

            Label {
                objectName: "queryLineError"
                Layout.fillWidth: true
                visible: controller ? controller.queryLineError.length > 0 : false
                text: controller ? controller.queryLineError : ""
                color: App.colour.warn
                font.pixelSize: App.smallTextSize
                wrapMode: Text.Wrap
            }
        }

        // Where the search is aimed, said rather than asked. Choosing is in More,
        // which keeps the picker, the editable root and the volume list -- the
        // folder is usually already right by the time somebody gets here, because
        // the tab was opened from it. See ADR-0067.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                text: "Searching"
                color: App.colour.textMuted
                font.pixelSize: App.secondaryTextSize
            }
            Label {
                objectName: "searchScopeText"
                Layout.fillWidth: true
                text: {
                    if (!controller)
                        return ""
                    if (!controller.everywhere)
                        return controller.rootUri
                    // Volume 0 is "All volumes", which adds nothing to the words
                    // in front of it.
                    var picked = controller.volumeIndex > 0
                        ? controller.volumeLabels[controller.volumeIndex] : ""
                    return picked.length > 0 ? "everywhere indexed \u2014 " + picked
                                             : "everywhere indexed"
                }
                color: App.colour.textSecondary
                elide: Text.ElideMiddle
                font.pixelSize: App.secondaryTextSize
            }
        }

        // One sentence about what this scope can be asked. It is what makes a
        // greyed field read as inapplicable rather than as broken.
        Label {
            objectName: "coverageNote"
            Layout.fillWidth: true
            // Nothing to say is no row: in the grid it used to sit in, an empty
            // label cost nothing, and in a column it leaves a hole above the
            // Search button.
            visible: text.length > 0
            text: controller ? controller.coverageNote : ""
            color: App.colour.textFaint
            font.pixelSize: App.smallTextSize
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ActionButton {
                text: controller && controller.running ? "Stop" : "Search"
                filled: !controller || !controller.running
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
                color: App.colour.textMuted
                elide: Text.ElideRight
                font.pixelSize: App.secondaryTextSize
            }
        }

        // A search that was asked something its scope cannot answer. Stopped
        // rather than quietly widened, with both ways out one click away.
        Rectangle {
            objectName: "blockedBanner"
            Layout.fillWidth: true
            visible: controller ? controller.blocked : false
            implicitHeight: blockedRow.implicitHeight + 12
            color: Qt.alpha(App.colour.warn, 0.16)
            border.color: App.colour.warn
            border.width: 1
            radius: 3

            RowLayout {
                id: blockedRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: controller ? controller.blockedReason : ""
                    color: App.colour.warn
                    font.pixelSize: App.secondaryTextSize
                    wrapMode: Text.Wrap
                }
                Button {
                    objectName: "indexThisFolderButton"
                    text: "Index this folder"
                    font.pixelSize: App.secondaryTextSize
                    onClicked: if (controller) controller.indexThisFolderForMetadata()
                }
                Button {
                    objectName: "narrowToIndexedButton"
                    text: "Search only the indexed part"
                    font.pixelSize: App.secondaryTextSize
                    visible: controller ? controller.hasIndexedPart : false
                    onClicked: if (controller) controller.narrowToIndexedPart()
                }
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
                color: App.colour.textMuted
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
                color: App.colour.textFaint
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
                text: (advancedArea.visible ? "▾  " : "▸  ") + "More"
                font.pixelSize: App.secondaryTextSize
                focusPolicy: Qt.NoFocus
                onClicked: advancedArea.visible = !advancedArea.visible
            }
            Item { Layout.fillWidth: true }
        }

        // The criteria scroll rather than run off the bottom. There are eleven rows
        // behind More now that the name fields joined them (ADR-0067), and in a
        // 900-tall window the last of them sat *under the task strip*: measured,
        // the grid reached y=880 and y=899 against a strip starting at 860, so the
        // size range and the "Use the index" toggle could not be clicked at all.
        // Nothing in this view scrolled. See MOLE-272.
        //
        // fillHeight is what lets it be squeezed rather than push the rows below it
        // off the window, and clip is what keeps the overflow out of sight until it
        // is scrolled to. maximumHeight earns its line separately and measurably:
        // without it the panel stretches to its share even when it holds less --
        // 551 px around 543 px of content with the scope set to everywhere -- and
        // the eight pixels are taken from the results.
        ScrollView {
            id: advancedArea
            objectName: "advancedArea"
            visible: false
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumHeight: advanced.implicitHeight

            // The criteria, in a component of their own since MOLE-168, because a
            // chain's filter step asks the same questions of the same controller.
            // The width is this host's answer; see ui/SearchCriteria.qml.
            SearchCriteria {
                id: advanced
                controller: view.controller
                width: advancedArea.availableWidth
            }
        }

        Label {
            Layout.fillWidth: true
            visible: controller ? controller.truncated : false
            text: "Result limit reached — this list is incomplete."
            color: App.colour.warn
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
                    color: App.colour.textFaint
                    font.pixelSize: App.smallTextSize
                }
            }
        }

        SearchResultList {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            canBuildSet: true
            // openPlace() rather than openFeatureTab(), and for the reason spelled
            // out at the other caller in DuplicatesView.qml: one Sets tab, pointed
            // at the set that was just built. See MOLE-254.
            onBuildSetRequested: {
                const id = controller.buildSetFromResults("")
                if (id.length > 0)
                    App.openPlace("set", id)
            }
            visible: !(controller && controller.running === true
                       && (controller.results ? controller.results.count === 0 : true))
            resultsModel: controller ? controller.results : null
        }
    }

    /// Opens the index dialog on `uri`, for *Index this folder* in the folder
    /// menu. That entry used to start a scan with none of these four questions
    /// asked, which made the discoverable door the poor one. See MOLE-228.
    function openIndexDialog(uri, label) {
        scanPath.text = uri
        scanLabel.text = label
        scanDialog.open()
    }

    Dialog {
        // A dialog sits on the panel ground, said here rather than inherited:
        // the window no longer hands one down. See ADR-0074.
        Material.background: App.colour.panel
        // Dimmed rather than washed out: Qt's Material dark theme dims with
        // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
        Overlay.modal: DimVeil {}
        Overlay.modeless: DimVeil {}

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

        // What was typed, as a uri. A bare path is a local one, and every
        // control in here has to agree about that -- the picker asking what a
        // folder is already on gets no answer for "/home/you/photos" if only
        // the accept handler knows it means "file:///home/you/photos".
        function scanUri() {
            return App.uriForPathText(scanPath.text)
        }

        onAccepted: {
            var uri = scanDialog.scanUri()
            if (controller && uri.length > 0) {
                controller.scanDirectory(uri, scanLabel.text, fullRescan.checked)
                // Always asked, because zero is a real answer: it is how
                // "Repeat: never" turns an existing nightly run off.
                controller.scheduleScan(uri, nightly.model[nightly.currentIndex].seconds)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Walks the tree once in the background and records it, so searching it later never touches the disk."
                color: App.colour.textMuted
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

            // Off by default, and the cost said in files rather than in
            // adjectives: this is one read per file, and the trees worth
            // indexing have a hundred thousand of them.
            CheckBox {
                objectName: "scanMetadataToggle"
                text: "Also record what each file says about itself"
                font.pixelSize: App.secondaryTextSize
                checked: controller ? controller.scanReadsMetadata : false
                onToggled: if (controller) controller.scanReadsMetadata = checked
            }
            // A re-scan keeps what has not changed, which is the difference
            // between minutes and hours on the trees this exists for. The full
            // one is what somebody reaches for when they suspect the index.
            CheckBox {
                objectName: "fullRescanToggle"
                id: fullRescan
                text: "Full rescan — walk everything, keep nothing"
                font.pixelSize: App.secondaryTextSize
            }
            // How often, and *whether* -- the same picker the report tab puts a
            // scan on a clock with, and the same words. It was a checkbox that
            // created a rule when ticked and did nothing at all when unticked,
            // over an interval written as 24 in the QML.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: "Keep it up to date"
                    font.pixelSize: App.secondaryTextSize
                }

                Picker {
                    objectName: "nightlyScanPicker"
                    id: nightly
                    font.pixelSize: App.secondaryTextSize
                    textRole: "text"
                    model: {
                        var out = [{ seconds: 0, text: "Repeat: never" }]
                        var presets = controller ? controller.schedulePresets() : []
                        for (var i = 0; i < presets.length; ++i)
                            out.push({ seconds: presets[i].seconds,
                                       text: "Repeat: " + presets[i].label.toLowerCase() })
                        return out
                    }
                    // The interval this folder is already on, so re-opening the
                    // dialog does not offer to turn a nightly run off by default.
                    currentIndex: {
                        var folder = scanDialog.scanUri()
                        if (!controller || folder.length === 0)
                            return 0
                        var on = controller.scheduledScanSeconds(folder)
                        for (var i = 0; i < model.length; ++i) {
                            if (model[i].seconds === on)
                                return i
                        }
                        return 0
                    }
                }

                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "A nightly run keeps what has not changed, so it costs a walk of what moved. "
                      + "It survives a restart and catches up on a night the machine was off."
                color: App.colour.textFaint
                font.pixelSize: App.smallTextSize
            }

            CheckBox {
                objectName: "scanArchivesToggle"
                text: "Also list what is inside archives"
                font.pixelSize: App.secondaryTextSize
                checked: controller ? controller.scanOpensArchives : true
                onToggled: if (controller) controller.scanOpensArchives = checked
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "A zip's own listing is one read; a tar.gz is a pass over the whole file. "
                      + "On a drive that is not local, listing one means fetching it, so large ones "
                      + "are left alone. Archives inside archives are listed and not opened."
                color: App.colour.textFaint
                font.pixelSize: App.smallTextSize
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Cameras, authors, durations — a few dozen bytes each, and one read per file. "
                      + "It makes the scan slower and lets you search for them afterwards. "
                      + "The contents themselves are never indexed."
                color: App.colour.textFaint
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
