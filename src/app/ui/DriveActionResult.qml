import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What a drive's own action answered.
//
// There are exactly two kinds of answer and this is both of them, because
// nothing in the window knows what the action was -- only which kind came back.
// Text is shown with a way to copy it and with whatever the drive said about how
// long it stays true; a list of other uris for the same file is offered as a list
// to open one from. Two backends answering with text are indistinguishable here,
// which is the whole point of the set being closed. See ADR-0075.
Dialog {
    id: dialog
    objectName: "driveActionResult"

    // A dialog sits on the panel ground, said here rather than inherited. ADR-0074.
    Material.background: App.colour.panel
    Overlay.modal: DimVeil {}
    Overlay.modeless: DimVeil {}

    modal: true
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 80 : 520)
    footer: ConfirmButtons { dismissOnly: true }

    /// "text" or "uris". Nothing else, ever.
    property string kind: "text"
    property string answer: ""
    /// Empty when the answer does not go stale, which is most of them.
    property string validUntil: ""
    property var choices: []
    /// Called with the uri of the entry that was picked.
    property var openRequested: function(uri) {}

    function showText(actionTitle, text, until) {
        dialog.title = actionTitle
        dialog.kind = "text"
        dialog.answer = text
        dialog.validUntil = until
        dialog.choices = []
        dialog.open()
    }

    function showChoices(actionTitle, entries) {
        dialog.title = actionTitle
        dialog.kind = "uris"
        dialog.answer = ""
        dialog.validUntil = ""
        dialog.choices = entries
        dialog.open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // --- text -------------------------------------------------------
        //
        // Shown and copyable, never opened in anything: Mole hands something to
        // another application in exactly one place, and only ever a local file.
        TextArea {
            id: answerText
            objectName: "driveActionText"
            visible: dialog.kind === "text"
            Layout.fillWidth: true
            readOnly: true
            wrapMode: TextArea.WrapAnywhere
            selectByMouse: true
            font.family: App.monospaceFont
            font.pixelSize: App.secondaryTextSize
            color: App.colour.text
            text: dialog.answer
        }

        RowLayout {
            visible: dialog.kind === "text"
            Layout.fillWidth: true
            spacing: 10

            Label {
                objectName: "driveActionValidUntil"
                visible: dialog.validUntil.length > 0
                Layout.fillWidth: true
                text: "Works until " + dialog.validUntil
                color: App.colour.textMuted
                font.pixelSize: App.smallTextSize
                wrapMode: Text.Wrap
            }

            Item { Layout.fillWidth: dialog.validUntil.length === 0 }

            Button {
                objectName: "driveActionCopy"
                text: "Copy"
                flat: true
                font.pixelSize: App.secondaryTextSize
                onClicked: {
                    answerText.selectAll()
                    answerText.copy()
                    answerText.deselect()
                }
            }
        }

        // --- other uris for the same file -------------------------------
        //
        // Each one opens as an ordinary file, because that is what it is.
        Label {
            visible: dialog.kind === "uris"
            Layout.fillWidth: true
            text: "Choose one to open it."
            color: App.colour.textMuted
            font.pixelSize: App.smallTextSize
        }

        ListView {
            objectName: "driveActionChoices"
            visible: dialog.kind === "uris"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(260, Math.max(1, dialog.choices.length) * 40)
            clip: true
            model: dialog.choices

            delegate: ItemDelegate {
                required property var modelData

                width: ListView.view.width
                text: modelData.label
                font.pixelSize: App.secondaryTextSize
                onClicked: {
                    dialog.openRequested(modelData.uri)
                    dialog.close()
                }
            }
        }
    }
}
