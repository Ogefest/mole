import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Adding and editing drives.
//
// The form is not written here. Every field — its name, its help, whether it is
// a password, whether it only applies to one provider — comes from the backend
// itself, so a new provider appears with a correct form and nothing in this file
// changes. That is the whole reason drives are a plugin seam.
Dialog {
    id: dialog
    objectName: "drivesDialog"

    property string editingId: ""
    property string factory: ""
    property string variant: ""
    property var values: ({})
    property bool showAdvanced: false

    title: "Drives"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(880, parent ? parent.width - 80 : 880)
    height: Math.min(640, parent ? parent.height - 80 : 640)
    standardButtons: Dialog.Close

    readonly property color mutedColor: "#8b93a7"
    readonly property color lineColor: "#2a3140"

    /// What the add button does. Kept apart from the plain reset below: the
    /// dialog already opens in the blank state, so resetting on its own changes
    /// nothing on screen -- which is exactly what pressing add used to look
    /// like. Opening the picker is the part the user can see. Saving resets
    /// without this, or the list of sixty backends would spring open every time
    /// a drive is stored.
    function beginAdding() {
        startNew()
        kindPicker.forceActiveFocus()
        kindPicker.popup.open()
    }

    function startNew() {
        editingId = ""
        factory = ""
        variant = ""
        values = ({})
        nameField.text = ""
        rootField.text = ""
        showAdvanced = false
    }

    function startEditing(drive) {
        editingId = drive.id
        factory = drive.factory
        variant = drive.variant
        values = Object.assign({}, drive.settings)
        nameField.text = drive.name
        rootField.text = drive.root
        showAdvanced = false
    }

    /// Always a string, whatever the backend put in the field description.
    /// Defaults arrive from the provider's own metadata, so they come as
    /// numbers, booleans, nulls and lists as well as text -- and a text field
    /// handed a list quietly refuses the whole binding, leaving the field
    /// blank and the console full of conversion warnings.
    function fieldValue(key, fallback) {
        const chosen = values[key] !== undefined ? values[key] : fallback
        if (chosen === undefined || chosen === null)
            return ""
        if (Array.isArray(chosen))
            return chosen.join(",")
        return String(chosen)
    }

    function setFieldValue(key, value) {
        var copy = Object.assign({}, values)
        copy[key] = value
        values = copy
    }

    // A field that only applies to one provider is hidden until that provider is
    // chosen. S3 has eighty options and no one form should show them all.
    function fieldApplies(field) {
        if (field.dependsOnKey === "")
            return true
        const current = fieldValue(field.dependsOnKey, "")
        return field.dependsOnValues.indexOf(current) >= 0
    }

    onOpened: startNew()

    // Assigned as the content item rather than anchored to the dialog. A Dialog
    // sizes itself from its content, so a layout that takes its size from the
    // dialog closes the loop -- and Qt, on detecting one, abandons the layout
    // and leaves every child with no size. The form was present and zero pixels
    // tall, which looks exactly like a button that does nothing.
    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // ---- the credential store -------------------------------------------

        Rectangle {
            Layout.fillWidth: true
            visible: App.credentialsAvailable && !App.credentialsUnlocked
            radius: 4
            color: "#2a2418"
            border.color: "#d9a441"
            implicitHeight: unlockRow.implicitHeight + 18

            ColumnLayout {
                id: unlockRow
                anchors.fill: parent
                anchors.margins: 9
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#e8c07d"
                    font.pixelSize: 11
                    text: App.credentialsExist
                          ? "Passwords are encrypted with a passphrase you choose. Enter it to "
                            + "use drives that need one."
                          : "Passwords are encrypted with a passphrase you choose. It is not "
                            + "stored anywhere, and it is not tied to this computer — back up "
                            + "the configuration and the same passphrase opens it on a fresh "
                            + "install."
                }

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: passphraseField
                        objectName: "passphraseField"
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: App.credentialsExist ? "Passphrase"
                                                              : "Choose a passphrase"
                        onAccepted: unlockButton.clicked()
                    }
                    Button {
                        id: unlockButton
                        objectName: "unlockButton"
                        text: App.credentialsExist ? "Unlock" : "Set"
                        enabled: passphraseField.text.length > 0
                        onClicked: {
                            if (App.unlockCredentials(passphraseField.text))
                                passphraseField.text = ""
                            else
                                unlockError.text = App.credentialsError()
                        }
                    }
                }

                Label {
                    id: unlockError
                    Layout.fillWidth: true
                    visible: text.length > 0
                    color: "#e5534b"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // ---- what is configured ------------------------------------------

            ColumnLayout {
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: "Your drives"
                        font.bold: true
                    }
                    ToolButton {
                        objectName: "addDriveButton"
                        text: "+"
                        font.pixelSize: App.textSize
                        onClicked: dialog.beginAdding()
                        ToolTip.visible: hovered
                        ToolTip.text: "Add a drive"
                    }
                }

                ListView {
                    objectName: "configuredDriveList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: App.configuredDrives

                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: 40
                        radius: 4
                        color: modelData.id === dialog.editingId ? "#26303f"
                             : driveMouse.containsMouse ? "#20262f" : "transparent"

                        MouseArea {
                            id: driveMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: dialog.startEditing(modelData)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: modelData.variant
                                          + (modelData.connected ? " · connected"
                                             : modelData.needsUnlock ? " · locked" : "")
                                    color: modelData.connected ? "#57ab5a"
                                         : modelData.needsUnlock ? "#d9a441" : dialog.mutedColor
                                    font.pixelSize: 10
                                }
                            }

                            ToolButton {
                                objectName: "driveConnectButton"
                                text: modelData.connected ? "⏏" : "▶"
                                implicitWidth: 24
                                implicitHeight: 24
                                onClicked: {
                                    if (modelData.connected) {
                                        App.disconnectDrive(modelData.id)
                                    } else {
                                        const problem = App.connectDrive(modelData.id)
                                        if (problem.length > 0)
                                            saveError.text = problem
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- the form ----------------------------------------------------

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "Kind"
                        color: dialog.mutedColor
                        font.pixelSize: 12
                    }
                    ComboBox {
                        id: kindPicker
                        objectName: "driveKindPicker"
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        textRole: "label"
                        model: App.driveKinds
                        // Sorted list of 45 backends; typing narrows it, because
                        // scrolling to "sftp" past forty others is not a thing to
                        // ask of anybody.
                        editable: true
                        // Sixty entries at full height covers the dialog, the
                        // window behind it and the drive list the user is
                        // choosing for. Capped from the list's own content
                        // height rather than the popup's implicit height: that
                        // is zero while the popup is shut, so capping against
                        // it collapses the dropdown to nothing and it never
                        // opens at all.
                        popup.height: Math.min(popup.contentItem.contentHeight + 16, 320)
                        onActivated: {
                            const kind = model[currentIndex]
                            dialog.factory = kind.factory
                            dialog.variant = kind.variant
                            dialog.values = ({})
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    color: dialog.mutedColor
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    text: {
                        const kinds = App.driveKinds
                        for (let i = 0; i < kinds.length; ++i) {
                            if (kinds[i].factory === dialog.factory
                                && kinds[i].variant === dialog.variant) {
                                return kinds[i].available ? kinds[i].description
                                                          : kinds[i].unavailableReason
                            }
                        }
                        return ""
                    }
                }

                // Something has to occupy the panel before a kind is picked,
                // or the right half of the dialog is blank and reads as broken
                // rather than as waiting.
                Label {
                    objectName: "drivePickPrompt"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: dialog.factory.length === 0
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: dialog.mutedColor
                    font.pixelSize: 12
                    text: "Choose a kind above to add a drive.\nPick one from the list or start typing its name."
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 4
                    visible: dialog.factory.length > 0

                    Label {
                        text: "Name"
                        color: dialog.mutedColor
                        font.pixelSize: 12
                    }
                    TextField {
                        id: nameField
                        objectName: "driveNameField"
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        placeholderText: "What to call it in the sidebar"
                    }

                    Label {
                        text: "Folder"
                        color: dialog.mutedColor
                        font.pixelSize: 12
                    }
                    TextField {
                        id: rootField
                        objectName: "driveRootField"
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        placeholderText: "Where inside the remote to start (blank means the top)"
                    }
                }

                // A view, not a column of items in a scroller.
                //
                // A layout inside a scroller whose width comes back from that
                // scroller re-enters itself: the scroller is given a size, the
                // column's width binding fires, and the column rearranges while
                // the outer rearrange is still on the stack. Switching backends
                // destroys every field while that is happening, and the layout
                // engine reads the attached Layout properties of an item that
                // is already going away. That was the segmentation fault, and
                // before it, the polish loop that left the form zero pixels
                // tall. A view owns its delegates, expects its model to change
                // under it, and never drives the layout that placed it.
                ListView {
                    id: fieldList
                    objectName: "driveFieldRepeater"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    visible: dialog.factory.length > 0
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    model: dialog.factory.length > 0
                           ? App.driveFields(dialog.factory, dialog.variant) : []

                    delegate: ColumnLayout {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        // Sized explicitly, because a view positions what it is
                        // told the height of. A field that does not apply takes
                        // no room rather than leaving a gap.
                        height: visible ? implicitHeight : 0
                        spacing: 1
                        visible: dialog.fieldApplies(modelData)
                                 && (!modelData.advanced || dialog.showAdvanced)

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.label
                                      + (modelData.required ? " *" : "")
                                font.pixelSize: 11
                                color: modelData.required ? "#d5dbe6" : dialog.mutedColor
                            }
                            Label {
                                visible: modelData.secret
                                text: "encrypted"
                                color: "#57ab5a"
                                font.pixelSize: 9
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // The control follows the field's kind, which
                        // the backend declared. Nothing here knows what
                        // an S3 storage class is.
                        Loader {
                            Layout.fillWidth: true
                            sourceComponent: modelData.kind === 1 ? secretField
                                           : modelData.kind === 3 ? booleanField
                                           : modelData.kind === 5 ? choiceField
                                                                  : plainField

                            property var field: modelData
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: modelData.help.length > 0
                            text: modelData.help
                            color: "#6f7788"
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: dialog.factory.length > 0

                    CheckBox {
                        text: "Show advanced options"
                        font.pixelSize: 11
                        checked: dialog.showAdvanced
                        onToggled: dialog.showAdvanced = checked
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        id: saveError
                        color: "#e5534b"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.maximumWidth: 300
                    }

                    Button {
                        visible: dialog.editingId.length > 0
                        text: "Delete"
                        flat: true
                        onClicked: {
                            App.removeDrive(dialog.editingId)
                            dialog.startNew()
                        }
                    }
                    Button {
                        objectName: "saveDriveButton"
                        text: "Save"
                        highlighted: true
                        enabled: nameField.text.trim().length > 0 && dialog.factory.length > 0
                        onClicked: {
                            saveError.text = ""
                            if (App.saveDrive(dialog.editingId, nameField.text, dialog.factory,
                                              dialog.variant, rootField.text, dialog.values)) {
                                dialog.startNew()
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: plainField
        TextField {
            font.pixelSize: 12
            text: dialog.fieldValue(field.key, field.defaultValue)
            onEditingFinished: dialog.setFieldValue(field.key, text)
        }
    }

    Component {
        id: secretField
        TextField {
            font.pixelSize: 12
            echoMode: TextInput.Password
            // Never pre-filled from the store. Showing a saved password back to
            // whoever is at the keyboard would undo the point of encrypting it.
            placeholderText: "unchanged"
            onEditingFinished: if (text.length > 0) dialog.setFieldValue(field.key, text)
        }
    }

    Component {
        id: booleanField
        CheckBox {
            font.pixelSize: 12
            checked: dialog.fieldValue(field.key, field.defaultValue) === "true"
            onToggled: dialog.setFieldValue(field.key, checked ? "true" : "false")
        }
    }

    Component {
        id: choiceField
        ComboBox {
            font.pixelSize: 12
            editable: true
            model: field.choices
            currentIndex: field.choices.indexOf(dialog.fieldValue(field.key,
                                                                  field.defaultValue))
            onActivated: dialog.setFieldValue(field.key, field.choices[currentIndex])
            onAccepted: dialog.setFieldValue(field.key, editText)
        }
    }
}
