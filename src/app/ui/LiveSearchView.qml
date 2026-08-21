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
                color: Material.color(Material.Amber)
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
                color: "#8b93a7"
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
                color: "#c8cede"
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
            color: "#6f7788"
            font.pixelSize: App.smallTextSize
            elide: Text.ElideRight
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

        // A search that was asked something its scope cannot answer. Stopped
        // rather than quietly widened, with both ways out one click away.
        Rectangle {
            objectName: "blockedBanner"
            Layout.fillWidth: true
            visible: controller ? controller.blocked : false
            implicitHeight: blockedRow.implicitHeight + 12
            color: "#231f16"
            border.color: "#4a3f22"
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
                    color: Material.color(Material.Amber)
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

            GridLayout {
                id: advanced
                objectName: "advancedCriteria"
                width: advancedArea.availableWidth
                columns: 6
                columnSpacing: 8
                rowSpacing: 6

                // Where to search is a field, not a tab. Searching everything ever
                // scanned used to be a separate window with its own form and its own
                // idea of what a search was; it is a scope, and the difference
                // between the two was only ever which engine could answer.
                //
                // Here rather than in front of More since ADR-0067: the basic view
                // says where the search is aimed and this is where it is chosen.
                // `everywhere:yes` on the line does the same thing.
                Label { text: "Search in"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                Picker {
                    objectName: "searchScope"
                    Layout.columnSpan: 2
                    Layout.preferredWidth: 200
                    model: ["This folder", "Everywhere indexed"]
                    currentIndex: controller && controller.everywhere ? 1 : 0
                    font.pixelSize: App.secondaryTextSize
                    onActivated: if (controller) controller.everywhere = (currentIndex === 1)
                }
                TextField {
                    objectName: "searchRootField"
                    Layout.fillWidth: true
                    Layout.columnSpan: 3
                    visible: !(controller && controller.everywhere)
                    text: controller ? controller.rootUri : ""
                    selectByMouse: true
                    font.pixelSize: App.secondaryTextSize
                    onEditingFinished: if (controller) controller.rootUri = text
                }
                // In its place when the scope is everywhere: which of the scanned
                // volumes, and how much each holds. The retired tab's one control.
                // Exactly one of the two is visible, which is what keeps this row at
                // six cells however the scope is set -- a GridLayout skips an
                // invisible item, so a row that could show neither would pull the
                // next one up into it.
                Picker {
                    objectName: "searchVolume"
                    Layout.fillWidth: true
                    Layout.columnSpan: 3
                    visible: controller && controller.everywhere === true
                    model: controller ? controller.volumeLabels : []
                    currentIndex: controller ? controller.volumeIndex : 0
                    font.pixelSize: App.secondaryTextSize
                    onActivated: if (controller) controller.volumeIndex = currentIndex
                }

                // The name, and what to make of it. Behind More since ADR-0067: the
                // line above says all three of these and more, so the basic view has
                // one box that takes a query rather than four.
                Label { text: "Name contains"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    id: queryField
                    objectName: "searchQueryField"
                    Layout.fillWidth: true
                    Layout.columnSpan: 4
                    text: controller ? controller.queryText : ""
                    selectByMouse: true
                    font.pixelSize: App.secondaryTextSize
                    onTextChanged: if (controller) controller.queryText = text
                    onAccepted: if (controller) controller.start()
                    // Down out of the box and into the results: once a search has
                    // answered, the answers are where the keyboard should be.
                    Keys.onDownPressed: resultList.takeFocus()
                }
                Picker {
                    objectName: "nameMode"
                    Layout.preferredWidth: 120
                    model: ["contains", "matches", "expression"]
                    currentIndex: controller ? controller.nameMode : 0
                    font.pixelSize: App.secondaryTextSize
                    onActivated: if (controller) controller.nameMode = currentIndex
                }

                Label { text: "Extension"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "extensionField"
                    Layout.columnSpan: 5
                    Layout.preferredWidth: 160
                    placeholderText: "jpg, jpeg, heic"
                    text: controller ? controller.extension : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextChanged: if (controller) controller.extension = text
                }

                // The other half of a search tool: the name is what you have
                // forgotten and the contents are what you remember. Last in the
                // form because it is last to be paid for.
                Label { text: "Text inside"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "contentField"
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    placeholderText: "words in the file itself"
                    text: controller ? controller.contentText : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextEdited: if (controller) controller.contentText = text
                }
                CheckBox {
                    objectName: "contentRegexToggle"
                    text: "expression"
                    font.pixelSize: App.secondaryTextSize
                    checked: controller ? controller.contentRegex : false
                    onToggled: if (controller) controller.contentRegex = checked
                }
                CheckBox {
                    objectName: "searchBinaryToggle"
                    text: "binary too"
                    font.pixelSize: App.secondaryTextSize
                    checked: controller ? controller.searchBinary : false
                    onToggled: if (controller) controller.searchBinary = checked
                }

                Label {
                    objectName: "contentCost"
                    Layout.columnSpan: 6
                    Layout.fillWidth: true
                    visible: controller ? controller.readsFileContents : false
                    text: "This one opens files, so narrow it with the criteria above first — "
                          + "the contents are never indexed."
                    color: "#6f7788"
                    font.pixelSize: App.smallTextSize
                    wrapMode: Text.Wrap
                }

                // Always visible, greyed when the scope has nothing recorded. A
                // missing field is a capability nobody ever discovers.
                Label { text: "It says"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                ColumnLayout {
                    objectName: "factCriteria"
                    Layout.columnSpan: 5
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: controller ? controller.factKeys : []
                        delegate: RowLayout {
                            required property string modelData
                            spacing: 6

                            Label {
                                Layout.preferredWidth: 130
                                text: modelData
                                color: "#8b93a7"
                                font.family: App.monospaceFont
                                font.pixelSize: App.smallTextSize
                            }
                            TextField {
                                objectName: "factField_" + modelData
                                Layout.preferredWidth: 220
                                font.pixelSize: App.secondaryTextSize
                                text: controller ? (controller.factCriteria[modelData] || "") : ""
                                onTextEdited: {
                                    if (!controller)
                                        return
                                    var all = Object.assign({}, controller.factCriteria)
                                    all[modelData] = text
                                    controller.factCriteria = all
                                }
                            }
                        }
                    }

                    Label {
                        objectName: "noMetadataHere"
                        Layout.fillWidth: true
                        visible: controller ? !controller.metadataAvailable : true
                        text: "Nothing here has been indexed for what the files say about themselves — "
                              + "scan this folder with that on and a camera, an author or a duration "
                              + "becomes something you can search for."
                        color: "#6f7788"
                        font.pixelSize: App.smallTextSize
                        wrapMode: Text.Wrap
                    }
                }

                Label { text: "Is a"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                Flow {
                    objectName: "typeClasses"
                    // Five, so this row fills the grid's six -- see searchQueryField above.
                    Layout.columnSpan: 5
                    Layout.fillWidth: true
                    spacing: 6

                    Repeater {
                        model: controller ? controller.allTypeClasses : []
                        delegate: CheckBox {
                            required property string modelData
                            text: modelData
                            font.pixelSize: App.secondaryTextSize
                            checked: controller && controller.typeClasses.indexOf(modelData) >= 0
                            onToggled: {
                                if (!controller)
                                    return
                                var picked = controller.typeClasses.slice()
                                const at = picked.indexOf(modelData)
                                if (checked && at < 0)
                                    picked.push(modelData)
                                else if (!checked && at >= 0)
                                    picked.splice(at, 1)
                                controller.typeClasses = picked
                            }
                        }
                    }
                }

                Label { text: "Changed"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "modifiedFromField"
                    Layout.preferredWidth: 130
                    placeholderText: "last 7 days"
                    text: controller ? controller.modifiedFrom : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextEdited: if (controller) controller.modifiedFrom = text
                }
                Label { text: "to"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "modifiedToField"
                    Layout.preferredWidth: 130
                    placeholderText: "today"
                    text: controller ? controller.modifiedTo : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextEdited: if (controller) controller.modifiedTo = text
                }
                Item { Layout.fillWidth: true; Layout.columnSpan: 2 }

                Label { text: "Path has"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "pathField"
                    Layout.columnSpan: 4
                    Layout.fillWidth: true
                    placeholderText: "invoices/2026"
                    text: controller ? controller.pathText : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextEdited: if (controller) controller.pathText = text
                }
                CheckBox {
                    objectName: "excludePathToggle"
                    text: "not"
                    font.pixelSize: App.secondaryTextSize
                    checked: controller ? controller.excludePath : false
                    onToggled: if (controller) controller.excludePath = checked
                }

                Label { text: "Skip folders"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                TextField {
                    objectName: "excludedField"
                    Layout.columnSpan: 5
                    Layout.fillWidth: true
                    placeholderText: "node_modules, .git, build"
                    text: controller ? controller.excluded : ""
                    font.pixelSize: App.secondaryTextSize
                    onTextEdited: if (controller) controller.excluded = text
                }

                Label { text: "Shape"; color: "#8b93a7"; font.pixelSize: App.secondaryTextSize }
                RowLayout {
                    Layout.columnSpan: 5
                    Layout.fillWidth: true
                    spacing: 8

                    Picker {
                        objectName: "kindMode"
                        Layout.preferredWidth: 150
                        model: ["files and folders", "files only", "folders only"]
                        currentIndex: controller ? controller.kindMode : 0
                        font.pixelSize: App.secondaryTextSize
                        onActivated: if (controller) controller.kindMode = currentIndex
                    }
                    CheckBox {
                        objectName: "wholeWordToggle"
                        text: "whole words"
                        font.pixelSize: App.secondaryTextSize
                        checked: controller ? controller.wholeWord : false
                        onToggled: if (controller) controller.wholeWord = checked
                    }
                    CheckBox {
                        objectName: "emptyOnlyToggle"
                        text: "empty only"
                        font.pixelSize: App.secondaryTextSize
                        checked: controller ? controller.emptyOnly : false
                        onToggled: if (controller) controller.emptyOnly = checked
                    }
                    CheckBox {
                        objectName: "hiddenToggle"
                        text: "hidden files"
                        font.pixelSize: App.secondaryTextSize
                        checked: controller ? controller.includeHidden : true
                        onToggled: if (controller) controller.includeHidden = checked
                    }
                    CheckBox {
                        objectName: "thisFolderOnlyToggle"
                        text: "this folder only"
                        font.pixelSize: App.secondaryTextSize
                        checked: controller ? controller.maxDepth === 0 : false
                        onToggled: if (controller) controller.maxDepth = checked ? 0 : -1
                    }
                    Item { Layout.fillWidth: true }
                }

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
                Item { Layout.fillWidth: true; Layout.columnSpan: 2 }

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
                    objectName: "unpushedNote"
                    Layout.columnSpan: 6
                    Layout.fillWidth: true
                    visible: controller ? controller.unpushedNote.length > 0 : false
                    text: controller ? controller.unpushedNote : ""
                    color: "#6f7788"
                    font.pixelSize: App.smallTextSize
                    wrapMode: Text.Wrap
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
                color: "#6f7788"
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
                color: "#6f7788"
                font.pixelSize: App.smallTextSize
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Cameras, authors, durations — a few dozen bytes each, and one read per file. "
                      + "It makes the scan slower and lets you search for them afterwards. "
                      + "The contents themselves are never indexed."
                color: "#6f7788"
                font.pixelSize: App.smallTextSize
            }
        }
    }
}
