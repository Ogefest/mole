import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The analysis tab.
//
// Charts are plain Rectangles on purpose: Qt Charts is GPL-or-commercial, and
// pulling it in would change this application's licence. Bars scaled to the
// largest row read better than a pie anyway when there are twenty extensions.
Item {
    id: view

    property var controller: null
    readonly property var target: controller ? controller.current : null
    /// Nothing has been scanned, so there is no report to show and the tab
    /// shows why instead. Named once because both halves of the view read it.
    readonly property bool nothingScannedYet: !target || (!target.hasReport && !target.busy)

    function focusActivePane() { body.forceActiveFocus() }
    Component.onCompleted: Qt.callLater(focusActivePane)

    // A labelled bar. Used for extensions, folders and histograms alike so
    // everything on the page is measured the same way.
    component MeterRow: RowLayout {
        id: meter
        required property string label
        required property string valueText
        required property double fraction
        property color barColor: Material.accent
        property string note: ""

        spacing: 8

        Label {
            Layout.preferredWidth: 130
            text: meter.label
            elide: Text.ElideMiddle
            font.pixelSize: 12
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 16
            radius: 2
            color: App.colour.hover

            Rectangle {
                width: Math.max(2, parent.width * Math.max(0, Math.min(1, meter.fraction)))
                height: parent.height
                radius: 2
                color: meter.barColor
            }
        }

        Label {
            Layout.preferredWidth: 90
            horizontalAlignment: Text.AlignRight
            text: meter.valueText
            color: App.colour.textMuted
            font.pixelSize: 11
        }

        Label {
            Layout.preferredWidth: 70
            horizontalAlignment: Text.AlignRight
            visible: meter.note.length > 0
            text: meter.note
            color: App.colour.textFaint
            font.pixelSize: 11
        }
    }

    component Card: Rectangle {
        default property alias content: inner.data
        property string heading: ""
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 34
        color: App.colour.panel
        radius: 4
        border.color: App.colour.border

        Label {
            x: 12
            y: 8
            text: parent.heading
            font.pixelSize: 11
            font.letterSpacing: 1
            color: App.colour.textMuted
        }
        ColumnLayout {
            id: inner
            x: 12
            y: 28
            width: parent.width - 24
            spacing: 4
        }
    }

    FocusScope {
        id: body
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // --- which folder, when several were selected -----------------
            ToolBar {
                Layout.fillWidth: true
                Material.background: App.colour.panel

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 6

                    Repeater {
                        model: controller ? controller.targets : []
                        delegate: Button {
                            required property var modelData
                            required property int index
                            text: modelData.label + (modelData.busy ? "  …" : "")
                            flat: controller.currentIndex !== index
                            highlighted: controller.currentIndex === index
                            font.pixelSize: 12
                            focusPolicy: Qt.NoFocus
                            onClicked: controller.currentIndex = index
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Picker {
                        Layout.preferredWidth: 230
                        visible: target && target.history.length > 1
                        font.pixelSize: 11
                        focusPolicy: Qt.NoFocus
                        textRole: "text"
                        model: {
                            if (!target)
                                return []
                            var out = [{ id: "", text: "Compare with…" }]
                            for (var i = 0; i < target.history.length; ++i) {
                                var h = target.history[i]
                                if (h.current)
                                    continue
                                out.push({ id: h.id, text: h.takenAt + "  ·  " + h.sizeText })
                            }
                            return out
                        }
                        onActivated: if (target) target.comparisonId = model[currentIndex].id
                    }

                    ToolButton {
                        text: target && target.busy ? "Scanning…" : "⟳  Rescan"
                        enabled: target && !target.busy
                        font.pixelSize: 12
                        focusPolicy: Qt.NoFocus
                        onClicked: controller.refreshAll()
                    }

                    // A report is worth far more as a series than as a
                    // snapshot, so repeating it is offered where it is made
                    // rather than buried in a settings screen.
                    Picker {
                        objectName: "repeatPicker"
                        font.pixelSize: 12
                        focusPolicy: Qt.NoFocus
                        textRole: "text"
                        model: {
                            var out = [{ seconds: 0, text: "Repeat: never" }]
                            var presets = target ? target.schedulePresets : []
                            for (var i = 0; i < presets.length; ++i)
                                out.push({ seconds: presets[i].seconds,
                                           text: "Repeat: " + presets[i].label.toLowerCase() })
                            return out
                        }
                        currentIndex: {
                            if (!target)
                                return 0
                            for (var i = 0; i < model.length; ++i) {
                                if (model[i].seconds === target.scheduleSeconds)
                                    return i
                            }
                            return 0
                        }
                        onActivated: if (target) target.setSchedule(model[currentIndex].seconds)
                    }
                }
            }

            // What the tab shows *instead of* a report, rather than the first
            // thing in a report that is not there. It was inside the scrolling
            // column, top-aligned, so it hung near the top of whatever height
            // the tab had and the emptiness underneath read as a mistake.
            Item {
                objectName: "analysisEmptyState"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: view.nothingScannedYet

                ColumnLayout {
                    objectName: "analysisEmptyStateBlock"
                    anchors.centerIn: parent
                    // Against the space it actually has. Centring inside a
                    // ScrollView's own width put it half the scrollbar off,
                    // because the scrollbar and the padding come off that
                    // width before the content sees it.
                    width: Math.min(parent.width - 80, 420)
                    spacing: 8

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "No report yet"
                        font.pixelSize: 16
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "Rescan walks the folder and files the result, so the next run has something to compare against."
                        color: App.colour.textMuted
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                    ActionButton {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Scan now"
                        focusPolicy: Qt.NoFocus
                        onClicked: controller.refreshAll()
                    }
                }
            }

            ScrollView {
                id: reportScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                visible: !view.nothingScannedYet
                contentWidth: availableWidth

                ColumnLayout {
                    // The ScrollView's available width, by name. Reaching two
                    // levels out through the parent chain got the control's own
                    // width, which is not the width its content has: the
                    // scrollbar and the padding come off first.
                    width: reportScroll.availableWidth
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: 40
                        visible: target && target.busy && !target.hasReport
                        spacing: 12
                        BusyIndicator { running: true }
                        Label {
                            text: target ? target.statusText : ""
                            color: App.colour.textMuted
                        }
                    }

                    // --- headline ----------------------------------------
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.margins: 10
                        visible: target && target.hasReport
                        columns: 5
                        columnSpacing: 8
                        rowSpacing: 8

                        Repeater {
                            model: {
                                if (!target || !target.hasReport)
                                    return []
                                var h = target.headline
                                return [
                                    { label: "Total size", value: h.sizeText },
                                    { label: "Files", value: h.files.toLocaleString(Qt.locale(), 'f', 0) },
                                    { label: "Folders", value: h.folders.toLocaleString(Qt.locale(), 'f', 0) },
                                    { label: "File kinds", value: h.kinds.toLocaleString(Qt.locale(), 'f', 0) },
                                    { label: "Average file", value: h.averageText }
                                ]
                            }
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 58
                                color: App.colour.panel
                                radius: 4
                                border.color: App.colour.border

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: modelData.value
                                        font.pixelSize: 17
                                        font.bold: true
                                        color: Material.accent
                                    }
                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: modelData.label
                                        font.pixelSize: 10
                                        color: App.colour.textMuted
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: target && target.hasReport && target.headline.partial
                        wrapMode: Text.Wrap
                        color: App.colour.warn
                        font.pixelSize: 12
                        text: target
                              ? "Incomplete: " + target.headline.unreadable
                                + " folder(s) could not be read, so these totals are a floor, not the truth."
                              : ""
                    }

                    // --- what changed ------------------------------------
                    Card {
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: target && target.hasDiff
                        heading: target && target.hasDiff
                                 ? "SINCE " + target.diffHeadline.since.toUpperCase() : "CHANGES"

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 20
                            Label {
                                text: target && target.hasDiff ? target.diffHeadline.bytesDeltaText : ""
                                font.pixelSize: 16
                                font.bold: true
                                color: target && target.hasDiff
                                       ? (target.diffHeadline.grew ? App.colour.warn
                                                                   : App.colour.ok)
                                       : App.colour.textMuted
                            }
                            Label {
                                text: {
                                    if (!target || !target.hasDiff)
                                        return ""
                                    var d = target.diffHeadline
                                    return (d.filesDelta >= 0 ? "+" : "")
                                         + App.countOf(d.filesDelta, "file", "files") + ", "
                                         + (d.foldersDelta >= 0 ? "+" : "")
                                         + App.countOf(d.foldersDelta, "folder", "folders")
                                }
                                color: App.colour.textMuted
                                font.pixelSize: 12
                            }
                        }

                        Repeater {
                            model: target && target.hasDiff ? target.diffRows : []
                            delegate: MeterRow {
                                required property var modelData
                                Layout.fillWidth: true
                                label: modelData.extension
                                valueText: (modelData.grew ? "+" : "−") + modelData.bytesDeltaText
                                fraction: modelData.peakShare
                                barColor: modelData.grew ? App.colour.warn
                                                         : App.colour.ok
                                note: modelData.isNew ? "new" : (modelData.isGone ? "gone" : "")
                            }
                        }

                        Label {
                            visible: target && target.hasDiff && target.diffRows.length === 0
                            text: "Nothing changed."
                            color: App.colour.textFaint
                            font.pixelSize: 12
                        }
                    }

                    // --- what it is made of ------------------------------
                    Card {
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: target && target.hasReport
                        heading: "WHAT IS IN HERE"

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            TextField {
                                objectName: "breakdownFilter"
                                Layout.preferredWidth: 160
                                placeholderText: "filter by extension"
                                font.pixelSize: 12
                                onTextChanged: if (target) target.extensions.filterText = text
                            }

                            Picker {
                                Layout.preferredWidth: 150
                                font.pixelSize: 11
                                focusPolicy: Qt.NoFocus
                                textRole: "text"
                                model: [
                                    { text: "any size", bytes: 0 },
                                    { text: "at least 1 MB", bytes: 1048576 },
                                    { text: "at least 10 MB", bytes: 10485760 },
                                    { text: "at least 100 MB", bytes: 104857600 }
                                ]
                                onActivated: if (target) target.extensions.minimumBytes = model[currentIndex].bytes
                            }

                            Switch {
                                text: "by count"
                                font.pixelSize: 11
                                focusPolicy: Qt.NoFocus
                                onToggled: if (target) target.extensions.byCount = checked
                            }

                            Item { Layout.fillWidth: true }

                            Label {
                                text: target
                                      ? target.extensions.count + " kinds shown"
                                        + (target.extensions.hiddenRows > 0
                                           ? ", " + target.extensions.hiddenRows + " hidden" : "")
                                      : ""
                                color: App.colour.textMuted
                                font.pixelSize: 11
                            }
                        }

                        // A single stacked bar: the shape of the folder at a
                        // glance, before any of the numbers are read.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            implicitHeight: 14
                            radius: 2
                            color: App.colour.hover
                            clip: true

                            Row {
                                anchors.fill: parent
                                Repeater {
                                    model: target ? target.extensions : null
                                    delegate: Rectangle {
                                        required property double share
                                        required property int index
                                        width: parent.width * share
                                        height: parent.height
                                        color: Qt.hsla((index * 0.13) % 1.0, 0.45, 0.58, 1.0)
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: target ? target.extensions : null
                            delegate: MeterRow {
                                required property string extension
                                required property string sizeText
                                required property int fileCount
                                required property double peakShare
                                required property int index
                                Layout.fillWidth: true
                                label: extension
                                valueText: sizeText
                                fraction: peakShare
                                barColor: Qt.hsla((index * 0.13) % 1.0, 0.45, 0.58, 1.0)
                                note: fileCount.toLocaleString(Qt.locale(), 'f', 0) + " ×"
                            }
                        }
                    }

                    // --- where it lives ----------------------------------
                    Card {
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: target && target.hasReport && target.topFolders.length > 0
                        heading: "BIGGEST SUBFOLDERS"

                        Repeater {
                            model: target ? target.topFolders : []
                            delegate: MeterRow {
                                required property var modelData
                                Layout.fillWidth: true
                                label: modelData.name
                                valueText: modelData.sizeText
                                fraction: modelData.peakShare
                                note: modelData.count.toLocaleString(Qt.locale(), 'f', 0) + " ×"
                            }
                        }
                    }

                    // --- distributions -----------------------------------
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: target && target.hasReport
                        spacing: 10

                        Card {
                            heading: "FILE SIZES"
                            Repeater {
                                model: target ? target.sizeBuckets : []
                                delegate: MeterRow {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    label: modelData.label
                                    valueText: modelData.count.toLocaleString(Qt.locale(), 'f', 0)
                                    fraction: modelData.peakShare
                                    barColor: App.colour.accent
                                }
                            }
                        }

                        Card {
                            heading: "LAST MODIFIED"
                            Repeater {
                                model: target ? target.ageBuckets : []
                                delegate: MeterRow {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    label: modelData.label
                                    valueText: modelData.count.toLocaleString(Qt.locale(), 'f', 0)
                                    fraction: modelData.peakShare
                                    barColor: App.colour.link
                                }
                            }
                        }
                    }

                    // --- the heavy ones ----------------------------------
                    Card {
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.bottomMargin: 12
                        visible: target && target.hasReport && target.largestFiles.length > 0
                        heading: "LARGEST FILES"

                        Repeater {
                            model: target ? target.largestFiles : []
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 12
                                }
                                Label {
                                    text: modelData.modifiedText
                                    color: App.colour.textFaint
                                    font.pixelSize: 11
                                }
                                Label {
                                    Layout.preferredWidth: 80
                                    horizontalAlignment: Text.AlignRight
                                    text: modelData.sizeText
                                    color: Material.accent
                                    font.pixelSize: 12
                                }
                                ToolButton {
                                    objectName: "analysisOpenFolderButton"
                                    text: "→"
                                    implicitWidth: App.minimumTarget
                                    implicitHeight: App.minimumTarget
                                    focusPolicy: Qt.NoFocus
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Show in the browser"
                                    onClicked: App.goTo(modelData.uri)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
