import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Renaming a batch, with the result visible before anything happens.
//
// The preview is the feature. Rules on the left in the order they apply, every
// file's before-and-after on the right, and Apply refuses outright while any row
// would collide — a batch that half-succeeds is worse than one that never ran.
Item {
    id: view
    property var controller: null

    readonly property color panelColor: "#1b2029"
    readonly property color lineColor: "#2a3140"
    readonly property color mutedColor: "#8b93a7"
    readonly property color badColor: "#e5534b"

    function focusActivePane() { body.forceActiveFocus() }

    FocusScope {
        id: body
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: "Bulk rename"
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    objectName: "renameSummary"
                    text: controller ? controller.summary : ""
                    color: controller && controller.blockedCount > 0 ? view.badColor
                                                                     : view.mutedColor
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: controller ? controller.busy : false
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }

                Button {
                    objectName: "applyRenameButton"
                    text: "Apply"
                    highlighted: true
                    enabled: controller ? controller.canApply : false
                    onClicked: controller.apply()
                    ToolTip.visible: hovered && controller && controller.blockedCount > 0
                    ToolTip.text: "Some rows would collide. Nothing is renamed until every one "
                                  + "of them can be."
                }
            }

            Label {
                Layout.fillWidth: true
                visible: !controller || controller.sourceCount === 0
                color: view.mutedColor
                wrapMode: Text.WordWrap
                text: "Nothing to rename.\n\n" +
                      "Select files in a browser tab, or open a set, then open this from " +
                      "Tools ▸ Bulk rename."
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: controller && controller.sourceCount > 0
                spacing: 12

                // ---- the rules, in the order they apply ----------------------

                // The rules are a form, and a form only needs the width its fields
                // need. Left to fill, the grids of full-width text boxes inside it
                // stretched a two-character prefix across a third of the window and
                // squeezed the preview -- which this view's own opening line calls
                // the feature -- into whatever was left.
                ColumnLayout {
                    Layout.preferredWidth: 360
                    Layout.maximumWidth: Math.max(360, Math.round(view.width * 0.38))
                    Layout.fillHeight: true
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "Rules"
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        ComboBox {
                            objectName: "addRulePicker"
                            implicitContentWidthPolicy: ComboBox.WidestText
                            font.pixelSize: App.secondaryTextSize
                            textRole: "label"
                            valueRole: "id"
                            model: controller ? controller.ruleKinds : []
                            displayText: "Add a rule…"
                            onActivated: controller.addRule(currentValue)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: controller && controller.rules.length === 0
                        color: view.mutedColor
                        wrapMode: Text.WordWrap
                        font.pixelSize: App.smallTextSize
                        text: "Rules apply in order, each to the result of the last. Stripping "
                              + "digits before numbering is a different result from numbering "
                              + "first, and both are useful."
                    }

                    ListView {
                        objectName: "renameRuleList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 6
                        model: controller ? controller.rules : []

                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: ruleBody.implicitHeight + 18
                            radius: 6
                            color: view.panelColor
                            border.width: 1
                            border.color: view.lineColor
                            opacity: modelData.enabled ? 1.0 : 0.55

                            ColumnLayout {
                                id: ruleBody
                                anchors.fill: parent
                                anchors.margins: 9
                                spacing: 6

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        text: (modelData.index + 1) + "."
                                        color: view.mutedColor
                                        font.pixelSize: App.smallTextSize
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        font.bold: true
                                        font.pixelSize: App.secondaryTextSize
                                    }
                                    ToolButton {
                                        text: "▲"
                                        implicitWidth: App.minimumTarget
                                        implicitHeight: App.minimumTarget
                                        enabled: modelData.index > 0
                                        onClicked: controller.moveRule(modelData.index, -1)
                                    }
                                    ToolButton {
                                        text: "▼"
                                        implicitWidth: App.minimumTarget
                                        implicitHeight: App.minimumTarget
                                        enabled: modelData.index < controller.rules.length - 1
                                        onClicked: controller.moveRule(modelData.index, 1)
                                    }
                                    Switch {
                                        checked: modelData.enabled
                                        onToggled: controller.setRuleEnabled(modelData.index, checked)
                                    }
                                    ToolButton {
                                        text: "×"
                                        font.pixelSize: App.textSize
                                        implicitWidth: App.minimumTarget
                                        implicitHeight: App.minimumTarget
                                        onClicked: controller.removeRule(modelData.index)
                                    }
                                }

                                // The fields each kind needs, and no others. The
                                // controller takes them by name, so a new field
                                // on a rule needs nothing here but a control.
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 2
                                    columnSpacing: 8
                                    rowSpacing: 4

                                    // --- replace ---
                                    TextField {
                                        visible: modelData.kind === "replace"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Find"
                                        text: modelData.find
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "find", text)
                                    }
                                    TextField {
                                        visible: modelData.kind === "replace"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Replace with"
                                        text: modelData.replaceWith
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "replaceWith", text)
                                    }
                                    CheckBox {
                                        visible: modelData.kind === "replace"
                                        text: "Pattern"
                                        font.pixelSize: App.smallTextSize
                                        checked: modelData.useRegex
                                        onToggled: controller.setRuleField(
                                            modelData.index, "useRegex", checked)
                                    }
                                    CheckBox {
                                        visible: modelData.kind === "replace"
                                        text: "Match case"
                                        font.pixelSize: App.smallTextSize
                                        checked: modelData.caseSensitive
                                        onToggled: controller.setRuleField(
                                            modelData.index, "caseSensitive", checked)
                                    }

                                    // --- case ---
                                    ComboBox {
                                        visible: modelData.kind === "case"
                                        Layout.columnSpan: 2
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        model: ["UPPER CASE", "lower case", "Title Case",
                                                "Sentence case"]
                                        currentIndex: modelData.caseStyle
                                        onActivated: controller.setRuleField(
                                            modelData.index, "caseStyle", currentIndex)
                                    }

                                    // --- insert / remove ---
                                    TextField {
                                        visible: modelData.kind === "insert"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Text"
                                        text: modelData.text
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "text", text)
                                    }
                                    SpinBox {
                                        visible: modelData.kind === "insert"
                                                 || modelData.kind === "remove"
                                        Layout.fillWidth: true
                                        from: -200
                                        to: 200
                                        value: modelData.position
                                        onValueModified: controller.setRuleField(
                                            modelData.index, "position", value)
                                    }
                                    SpinBox {
                                        visible: modelData.kind === "remove"
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 200
                                        value: modelData.length
                                        onValueModified: controller.setRuleField(
                                            modelData.index, "length", value)
                                    }

                                    // --- strip ---
                                    ComboBox {
                                        visible: modelData.kind === "strip"
                                        Layout.columnSpan: 2
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        model: ["Digits", "Punctuation", "Spaces", "Accents",
                                                "Non-ASCII"]
                                        currentIndex: modelData.stripClass
                                        onActivated: controller.setRuleField(
                                            modelData.index, "stripClass", currentIndex)
                                    }

                                    // --- number ---
                                    SpinBox {
                                        visible: modelData.kind === "number"
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 100000
                                        value: modelData.start
                                        onValueModified: controller.setRuleField(
                                            modelData.index, "start", value)
                                    }
                                    SpinBox {
                                        visible: modelData.kind === "number"
                                        Layout.fillWidth: true
                                        from: 1
                                        to: 1000
                                        value: modelData.step
                                        onValueModified: controller.setRuleField(
                                            modelData.index, "step", value)
                                    }
                                    SpinBox {
                                        visible: modelData.kind === "number"
                                        Layout.fillWidth: true
                                        from: 1
                                        to: 10
                                        value: modelData.padding
                                        onValueModified: controller.setRuleField(
                                            modelData.index, "padding", value)
                                    }
                                    TextField {
                                        visible: modelData.kind === "number"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Separator"
                                        text: modelData.numberSeparator
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "numberSeparator", text)
                                    }

                                    // --- affix ---
                                    TextField {
                                        objectName: "rulePrefixField"
                                        visible: modelData.kind === "affix"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Prefix"
                                        text: modelData.prefix
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "prefix", text)
                                    }
                                    TextField {
                                        visible: modelData.kind === "affix"
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "Suffix"
                                        text: modelData.suffix
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "suffix", text)
                                    }

                                    // --- extension ---
                                    TextField {
                                        visible: modelData.kind === "extension"
                                        Layout.columnSpan: 2
                                        Layout.fillWidth: true
                                        font.pixelSize: App.secondaryTextSize
                                        placeholderText: "New extension (empty lower-cases it)"
                                        text: modelData.newExtension
                                        onTextEdited: controller.setRuleField(
                                            modelData.index, "newExtension", text)
                                    }

                                    // Which part of the name this rule touches.
                                    // Stem by default: upper-casing a name should
                                    // not turn ".txt" into something no tool knows.
                                    ComboBox {
                                        visible: modelData.kind !== "extension"
                                        Layout.columnSpan: 2
                                        Layout.fillWidth: true
                                        font.pixelSize: App.smallTextSize
                                        model: ["Name only", "Extension only", "Whole filename"]
                                        currentIndex: modelData.scope
                                        onActivated: controller.setRuleField(
                                            modelData.index, "scope", currentIndex)
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- what it would do ---------------------------------------

                // And the preview keeps a floor of its own, so no arrangement of
                // rules can crowd it out.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 320
                    Layout.fillHeight: true
                    spacing: 6

                    Label {
                        text: "Preview"
                        font.bold: true
                        font.pixelSize: App.textSize
                    }

                    ListView {
                        objectName: "renamePreviewList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: controller ? controller.preview : []

                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: modelData.blocked ? 40 : 24
                            color: modelData.blocked ? "#2a1f1f" : "transparent"

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Label {
                                        Layout.preferredWidth: 200
                                        text: modelData.from
                                        elide: Text.ElideMiddle
                                        font.family: App.monospaceFont
                                        font.pixelSize: App.smallTextSize
                                        color: modelData.changed ? view.mutedColor : "#5c6472"
                                    }
                                    Label {
                                        text: modelData.changed ? "→" : "="
                                        color: "#4a5364"
                                        font.pixelSize: App.smallTextSize
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.to
                                        elide: Text.ElideMiddle
                                        font.family: App.monospaceFont
                                        font.pixelSize: App.smallTextSize
                                        color: modelData.blocked ? view.badColor
                                             : modelData.changed ? "#a5d6a7" : "#5c6472"
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: modelData.blocked
                                    text: modelData.problem
                                    color: view.badColor
                                    font.pixelSize: App.smallTextSize
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
