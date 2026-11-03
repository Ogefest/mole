import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every index that exists, and how much any of them can be trusted.
//
// An index is a claim about a tree that goes quietly out of date. The only place
// one used to be visible was a dropdown inside the search form, as a label and a
// count — so how old a search's answer was, and whether it could answer a
// question about a camera at all, was decided by something nobody could look at.
//
// One row per index, stalest first, and every column is a question somebody
// actually has: what it covers, how old it is, how big it is, what kind of scan
// built it, and whether anything is keeping it fresh.
Item {
    id: view
    property var controller: null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "Indexes"
                font.pixelSize: 20
                font.bold: true
            }

            Label {
                objectName: "indexesSummary"
                visible: controller && controller.volumeCount > 0
                color: App.colour.textMuted
                text: controller
                      ? controller.volumeCount + (controller.volumeCount === 1 ? " index · " : " indexes · ")
                        + controller.totalEntriesText + " entries · "
                        + (controller.scheduledCount > 0
                           ? controller.scheduledCount + " kept up to date"
                           : "none kept up to date")
                      : ""
            }

            Item { Layout.fillWidth: true }

            TextField {
                objectName: "indexFilter"
                Layout.preferredWidth: 240
                visible: controller && controller.volumeCount > 0
                font.pixelSize: 12
                placeholderText: "Filter indexes…"
                text: controller ? controller.filter : ""
                onTextEdited: if (controller) controller.filter = text
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !controller || controller.volumeCount === 0
            color: App.colour.textMuted
            wrapMode: Text.WordWrap
            text: "Nothing is indexed yet.\n\n" +
                  "Index a folder — Operations ▸ Index this folder, or the button in a search tab — and " +
                  "searching it afterwards never touches the disk again. That is the difference " +
                  "between instant and a walk of the whole tree on a slow drive.\n\n" +
                  "An index goes out of date on its own, so put one on a clock and it keeps itself " +
                  "current: a nightly run costs a walk of what moved rather than of everything."
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controller && controller.volumeCount > 0
            radius: 6
            color: App.colour.panel
            border.width: 1
            border.color: App.colour.border

            ListView {
                objectName: "indexList"
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 2
                model: controller ? controller.volumes : []

                // Handed to every row from here, where they are evaluated in this
                // view's own scope. See the row below for why a row may not look
                // them up by name.
                readonly property var rowController: view.controller
                readonly property var repeatChoices: {
                    var out = [{ seconds: 0, text: "Repeat: never" }]
                    var presets = view.controller ? view.controller.schedulePresets() : []
                    for (var i = 0; i < presets.length; ++i)
                        out.push({ seconds: presets[i].seconds,
                                   text: "Repeat: " + presets[i].label.toLowerCase() })
                    return out
                }

                // **Everything a row needs from outside itself, it gets through
                // ListView.view and carries as a property of its own.** A name
                // inside a row is looked up in the row's own context first and
                // then in the one the ListView gives each delegate -- and on Qt
                // 6.11 that one's context object is the row's model entry
                // (QQmlDMListAccessorData), which answers *every* name with
                // undefined while the row is being made. Not only `controller`:
                // `view`, `App`, `JSON` and `console` all came back undefined on
                // the picker's first evaluation, so its list was "never" alone, no
                // dependency on the real controller was recorded, and nothing ever
                // evaluated it again. Qt 6.4 asked the outer scopes and was right.
                // `controller` declared here is found before that context is ever
                // reached, so every `controller` in the row means this one. See
                // MOLE-412.
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property var controller: ListView.view ? ListView.view.rowController : null
                    readonly property var repeatChoices: ListView.view ? ListView.view.repeatChoices : []
                    width: ListView.view.width
                    implicitHeight: rowBody.implicitHeight + 14
                    radius: 4
                    color: rowMouse.containsMouse ? App.colour.hover : "transparent"

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        // The tree itself, which is what somebody looking at a
                        // stale index usually wants next.
                        onDoubleClicked: App.openLocation(modelData.rootUri)
                    }

                    ColumnLayout {
                        id: rowBody
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                text: modelData.label
                                font.bold: true
                                font.pixelSize: 13
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: App.countOf(modelData.entryCount, "entry", "entries")
                                color: App.colour.textMuted
                                font.pixelSize: 10
                            }
                            Item { Layout.fillWidth: true }
                            // Said in its own colour when nothing is keeping it
                            // fresh, because that is the row's real news.
                            Label {
                                objectName: "indexScheduleText"
                                text: modelData.scheduled
                                      ? modelData.scheduleText + " · next " + modelData.nextDueText
                                      : modelData.scheduleText
                                color: modelData.scheduled ? App.colour.textMuted : App.colour.warn
                                font.pixelSize: 11
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: modelData.rootUri
                            color: App.colour.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideMiddle
                        }

                        Label {
                            Layout.fillWidth: true
                            text: modelData.scannedText + " · " + modelData.kindText
                            color: modelData.kindKnown ? App.colour.textMuted : App.colour.warn
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        // What can be done to it, on the row rather than in a
                        // menu somewhere: re-indexing a tree used to mean typing
                        // its path back into the index dialog from memory.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            // A scan that is running takes the row over, because
                            // then the only useful action is stopping it.
                            Label {
                                objectName: "indexProgressText"
                                visible: modelData.running
                                Layout.fillWidth: true
                                text: modelData.progressText.length > 0
                                      ? "Scanning — " + modelData.progressText
                                      : "Scanning…"
                                color: App.colour.textMuted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Button {
                                objectName: "indexStopButton"
                                visible: modelData.running
                                text: "Stop"
                                flat: true
                                font.pixelSize: 11
                                // Nothing is lost: what the volume held before
                                // stays searchable and its date does not move.
                                onClicked: controller.stopScan(modelData.id)
                            }

                            Button {
                                objectName: "indexRescanButton"
                                visible: !modelData.running
                                text: "Rescan"
                                flat: true
                                font.pixelSize: 11
                                onClicked: controller.rescan(modelData.id, false)
                            }
                            Button {
                                objectName: "indexFullRescanButton"
                                visible: !modelData.running
                                text: "Full rescan"
                                flat: true
                                font.pixelSize: 11
                                onClicked: controller.rescan(modelData.id, true)
                            }

                            // The same picker the index dialog offers, over the
                            // same presets, so the two places agree.
                            Picker {
                                objectName: "indexRepeatPicker"
                                visible: !modelData.running
                                font.pixelSize: 11
                                textRole: "text"
                                // Through the row, never by a bare name -- see the
                                // delegate above for what a bare name found here.
                                model: row.repeatChoices
                                currentIndex: {
                                    var on = row.modelData.scheduled ? row.modelData.scheduleSeconds : 0
                                    for (var i = 0; i < model.length; ++i) {
                                        if (model[i].seconds === on)
                                            return i
                                    }
                                    return 0
                                }
                                onActivated: if (row.controller)
                                                 row.controller.setSchedule(row.modelData.id,
                                                                            model[currentIndex].seconds)
                            }

                            Item { Layout.fillWidth: !modelData.running }

                            Button {
                                objectName: "indexForgetButton"
                                visible: !modelData.running
                                text: "Forget"
                                flat: true
                                font.pixelSize: 11
                                onClicked: {
                                    forgetDialog.volumeId = modelData.id
                                    forgetDialog.volumeLabel = modelData.label
                                    forgetDialog.open()
                                }
                            }
                        }
                    }
                }
            }
        }

        // The other half of the answer, and a different question: this tab lists
        // what exists, Automation lists what runs by itself. An index with no
        // rule does not appear there at all, which is why the two are not one.
        RowLayout {
            Layout.fillWidth: true
            visible: controller && controller.volumeCount > 0
            spacing: 8

            Label {
                Layout.fillWidth: true
                color: App.colour.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                text: "An index is only as fresh as its last scan. Put one on a clock and it keeps " +
                      "itself current: a repeat costs a walk of what moved rather than of everything."
            }
            Button {
                objectName: "openAutomationButton"
                text: "Automation"
                flat: true
                // The menu's own entry rather than a second way in. It opened the tab
                // directly, which meant two routes to the same place and only one of
                // them going through openStandingTab() -- so this button left a second
                // Schedule tab after the menu had stopped doing that. Triggering the
                // action means there is one route, and the test that walks the action
                // registry covers this button without knowing it exists. See MOLE-259.
                onClicked: App.triggerAction("mole.tools.automation")
            }
        }
    }

    MoleDialog {
        id: forgetDialog
        objectName: "forgetIndexDialog"
        // Without this the popup never becomes a focus scope, so nothing inside
        // it can hold the keyboard and the footer's focus quietly does nothing.
        title: "Forget this index?"

        property var volumeId: -1
        property string volumeLabel: ""

        // Hours of walking, thrown away on a click. Destructive, and told apart
        // from the way out — ADR-0010.
        footer: ConfirmButtons {
            acceptText: "Forget"
            destructive: true
        }

        onAccepted: if (controller) controller.forget(forgetDialog.volumeId)

        Label {
            width: 380
            wrapMode: Text.Wrap
            text: "The index of " + forgetDialog.volumeLabel + " is deleted, and any schedule " +
                  "keeping it up to date goes with it.\n\nNo files are touched. An index holds " +
                  "nothing that is not already in them, so this costs a rescan and nothing else — " +
                  "though on a large tree that rescan is not quick."
        }
    }
}
