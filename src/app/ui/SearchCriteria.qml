import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every criterion a search can carry, asked once and hosted twice.
//
// This was thirty-five fields inside LiveSearchView.qml until MOLE-168. A chain's
// filter step asks the same questions of the same controller -- narrowing a list is
// the commonest thing anybody does between two operations -- and the alternative to
// one component was two copies of this grid, which would have disagreed inside a
// week: a criterion added to one and not the other is a search that can express
// something a chain cannot, or the reverse, and nothing would have said so.
//
// **A component and not a generated form.** The other candidates were building this
// from StepParameter declarations, and opening the search tab in a configuring mode.
// The first turns thirty-five deliberate layout decisions into a table nobody can
// tune; the second makes a tab a dialog and gives the chain editor a tab's keyboard.
// See the epic's note on the interface question, which is where this resolved.
//
// **The host owns the width.** Inside the search view it is a ScrollView's
// availableWidth; a chain editor will have its own answer. Nothing else here reaches
// outside itself, which is what made the extraction a move rather than a rewrite.
//
// `objectName` is unchanged on every field, so every test that drove the search
// view before this drives it still -- which is the assertion that this changed
// nothing about the search.
GridLayout {
    id: criteria
    objectName: "advancedCriteria"

    /// The LiveSearchController whose criteria these are. Every field below reads
    /// and writes it, and it is the only thing this component knows about.
    required property var controller

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
    Label { text: "Search in"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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
    Label { text: "Name contains"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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

    Label { text: "Extension"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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
    Label { text: "Text inside"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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
        color: App.colour.textFaint
        font.pixelSize: App.smallTextSize
        wrapMode: Text.Wrap
    }

    // Always visible, greyed when the scope has nothing recorded. A
    // missing field is a capability nobody ever discovers.
    Label { text: "It says"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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
                    color: App.colour.textMuted
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
            color: App.colour.textFaint
            font.pixelSize: App.smallTextSize
            wrapMode: Text.Wrap
        }
    }

    Label { text: "Is a"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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

    Label { text: "Changed"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
    TextField {
        objectName: "modifiedFromField"
        Layout.preferredWidth: 130
        placeholderText: "last 7 days"
        text: controller ? controller.modifiedFrom : ""
        font.pixelSize: App.secondaryTextSize
        onTextEdited: if (controller) controller.modifiedFrom = text
    }
    Label { text: "to"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
    TextField {
        objectName: "modifiedToField"
        Layout.preferredWidth: 130
        placeholderText: "today"
        text: controller ? controller.modifiedTo : ""
        font.pixelSize: App.secondaryTextSize
        onTextEdited: if (controller) controller.modifiedTo = text
    }
    Item { Layout.fillWidth: true; Layout.columnSpan: 2 }

    Label { text: "Path has"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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

    Label { text: "Skip folders"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
    TextField {
        objectName: "excludedField"
        Layout.columnSpan: 5
        Layout.fillWidth: true
        placeholderText: "node_modules, .git, build"
        text: controller ? controller.excluded : ""
        font.pixelSize: App.secondaryTextSize
        onTextEdited: if (controller) controller.excluded = text
    }

    Label { text: "Shape"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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

    Label { text: "Size from"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
    TextField {
        id: minSizeField
        objectName: "minSizeField"
        Layout.preferredWidth: 110
        placeholderText: "10M"
        font.pixelSize: App.secondaryTextSize
        onTextEdited: if (controller) controller.setSizeRange(minSizeField.text, maxSizeField.text)
    }
    Label { text: "to"; color: App.colour.textMuted; font.pixelSize: App.secondaryTextSize }
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
        color: App.colour.textFaint
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
        color: App.colour.textFaint
        font.pixelSize: App.smallTextSize
        elide: Text.ElideRight
    }
}
