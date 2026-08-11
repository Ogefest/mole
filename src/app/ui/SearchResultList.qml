import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shared result list for every search flavour. Rows carry a full uri, so
// "reveal in browser" works whether the hit came from disk or from the index.
Rectangle {
    id: results

    property var resultsModel: null
    /// Whether to offer building a file set from what was found. The live search
    /// can; the index search has nowhere to put one yet.
    property bool canBuildSet: false

    signal buildSetRequested()

    /// Called by the view when the keyboard should come here.
    function takeFocus() {
        if (list.currentIndex < 0 && list.count > 0)
            list.currentIndex = 0
        list.forceActiveFocus()
    }

    function revealCurrent() {
        if (list.currentIndex < 0 || !resultsModel)
            return
        const uri = resultsModel.uriAt(list.currentIndex)
        if (uri.length === 0)
            return
        // A folder is opened; a file has its folder opened with the cursor on it,
        // which is what "show me where this is" means.
        if (resultsModel.isDirAt(list.currentIndex))
            App.goTo(uri)
        else
            App.revealFile(uri)
    }

    function previewCurrent() {
        if (list.currentIndex < 0 || !resultsModel)
            return
        if (!resultsModel.isDirAt(list.currentIndex))
            App.previewFile(resultsModel.uriAt(list.currentIndex))
    }

    color: "#151922"
    border.color: "#2a3140"
    border.width: 1

    // What can be done with what was found. Beside the results rather than beside
    // the criteria: these act on the rows, so they belong with them.
    ToolBar {
        id: actions
        objectName: "resultActions"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 1
        visible: results.resultsModel ? results.resultsModel.count > 0 : false

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 6

            ToolButton {
                objectName: "revealButton"
                text: "Show in folder"
                font.pixelSize: App.secondaryTextSize
                focusPolicy: Qt.NoFocus
                enabled: list.currentIndex >= 0
                ToolTip.text: "Open the folder this is in, with the cursor on it  (Enter)"
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: results.revealCurrent()
            }
            ToolButton {
                objectName: "previewResultButton"
                text: "Preview"
                font.pixelSize: App.secondaryTextSize
                focusPolicy: Qt.NoFocus
                enabled: list.currentIndex >= 0
                ToolTip.text: "Look at this without leaving the results  (F3)"
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: results.previewCurrent()
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                objectName: "buildSetFromResultsButton"
                visible: results.canBuildSet
                text: "Build a set"
                font.pixelSize: App.secondaryTextSize
                focusPolicy: Qt.NoFocus
                ToolTip.text: "Make a file set of these results, to keep working on them"
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: results.buildSetRequested()
            }
        }
    }

    ListView {
        id: list
        objectName: "searchResults"
        anchors.top: actions.visible ? actions.bottom : parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        clip: true
        model: results.resultsModel
        reuseItems: true
        // Walking the results should feel like walking a listing: arrows move,
        // Enter goes there, F3 looks at it without leaving.
        focus: true
        keyNavigationEnabled: true
        currentIndex: 0
        highlightMoveDuration: 0
        // Results arrive after this view exists, and a model that had no rows
        // leaves the cursor at -1. Arriving at a list of answers should put it on
        // the first one rather than nowhere.
        onCountChanged: if (currentIndex < 0 && count > 0) currentIndex = 0
        ScrollBar.vertical: ScrollBar {}

        Keys.onReturnPressed: results.revealCurrent()
        Keys.onEnterPressed: results.revealCurrent()
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_F3) {
                results.previewCurrent()
                event.accepted = true
            }
        }

        delegate: ItemDelegate {
            required property int index
            required property string name
            required property string uri
            required property string parentUri
            required property bool isDir
            required property string sizeText
            required property string iconText
            /// 0 seen now, 1 from a previous scan. See FileListModel::Provenance.
            required property int provenance
            required property var indexedAt
            required property string matchLine
            required property int matchLineNumber

            width: ListView.view.width
            height: 34
            highlighted: ListView.isCurrentItem
            onClicked: list.currentIndex = index
            onDoubleClicked: {
                list.currentIndex = index
                results.revealCurrent()
            }

            background: Rectangle {
                color: parent.highlighted ? "#232a36" : (hovered ? "#1d232e" : "transparent")
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8

                Label { text: iconText; font.pixelSize: App.textSize }

                // A row a scan remembered rather than one anything has just
                // looked at. Quiet, because most rows in a mixed list are this
                // for a moment and then stop being it; and marked at all,
                // because a list that mixes the two without saying which is an
                // answer nobody can reason about. See ADR-0038.
                Label {
                    objectName: "rememberedMarker"
                    visible: provenance === 1
                    text: "\u25CB" // an open circle: seen once, not just now
                    color: "#8b93a7"
                    font.pixelSize: App.smallTextSize
                    ToolTip.visible: hovered.hovered
                    ToolTip.text: indexedAt && !isNaN(indexedAt.getTime())
                                  ? "From the index, scanned " + indexedAt.toLocaleString(Qt.locale())
                                  : "From the index"
                    HoverHandler { id: hovered }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: name
                        elide: Text.ElideMiddle
                        font.pixelSize: App.textSize
                    }
                    // Why this is a hit, when the search asked what is inside
                    // the file. A list of names would make somebody open every
                    // one of them to find out which it meant.
                    Label {
                        objectName: "matchLine"
                        Layout.fillWidth: true
                        visible: matchLine.length > 0
                        text: matchLineNumber + ":  " + matchLine
                        elide: Text.ElideRight
                        color: "#8b93a7"
                        font.family: App.monospaceFont
                        font.pixelSize: App.smallTextSize
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: matchLine.length === 0
                        text: parentUri
                        elide: Text.ElideLeft
                        color: "#6f7788"
                        font.pixelSize: App.smallTextSize
                    }
                }

                Label {
                    Layout.preferredWidth: 80
                    horizontalAlignment: Text.AlignRight
                    text: sizeText
                    color: "#8b93a7"
                    font.pixelSize: App.secondaryTextSize
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !results.resultsModel || results.resultsModel.count === 0
        text: "No results yet"
        color: "#6f7788"
    }
}
