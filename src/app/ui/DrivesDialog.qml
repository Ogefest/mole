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
    // A dialog sits on the panel ground, said here rather than inherited:
    // the window no longer hands one down. See ADR-0074.
    Material.background: App.colour.panel
    // Dimmed rather than washed out: Qt's Material dark theme dims with
    // near-white at sixty percent. See ui/DimVeil.qml and MOLE-128.
    Overlay.modal: DimVeil {}
    Overlay.modeless: DimVeil {}

    id: dialog
    objectName: "drivesDialog"

    property string editingId: ""
    property string factory: ""
    property string variant: ""
    property var values: ({})
    property bool showAdvanced: false

    // The outcome of the last reachability check. A configuration cannot be told
    // apart from a wrong one by looking at it, so the answer is kept in front of
    // whoever typed it instead of only going to the notification area.
    property string checkMessage: ""
    property bool checkOk: false
    /// The drive a sweep reported leftovers for, and how many. Held so the
    /// banner can offer to clear them: what was found is somebody's, and one of
    /// them may be a copy running on another machine right now, so finding and
    /// removing are two steps rather than one.
    property string sweptDriveId: ""
    property int sweptFound: 0

    Connections {
        target: App
        function onDriveSwept(id, found, cleared, message) {
            dialog.checkOk = true
            dialog.checkMessage = message
            // Only an unremoved find leaves the offer standing.
            dialog.sweptDriveId = (found > 0 && !cleared) ? id : ""
            dialog.sweptFound = cleared ? 0 : found
        }
        function onDriveChecked(id, reachable, message) {
            dialog.checkOk = reachable
            dialog.checkMessage = message
            dialog.sweptDriveId = ""
            dialog.sweptFound = 0
        }
    }

    title: "Drives"
    modal: true
    // Without this the popup never becomes a focus scope, so nothing inside it
    // can hold the keyboard and the footer's focus quietly does nothing.
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(880, parent ? parent.width - 80 : 880)
    height: Math.min(640, parent ? parent.height - 80 : 640)

    // Nothing to confirm: a drive is saved by the form's own button, so the one
    // button here is the way out. It holds the keyboard all the same.
    footer: ConfirmButtons { dismissOnly: true }

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

    /// Which row of `App.driveKinds` a kind is, or -1 when it is not one of them.
    ///
    /// -1 rather than 0 for "no match", because row zero is a real backend and a
    /// form that is not editing anything must not name one.
    function indexOfKind(factoryScheme, variantName) {
        const kinds = App.driveKinds
        for (let i = 0; i < kinds.length; ++i) {
            if (kinds[i].factory === factoryScheme && kinds[i].variant === variantName)
                return i
        }
        return -1
    }

    /// Points the Kind picker at what the form is showing.
    ///
    /// **Set here rather than bound**, which is where every other control in this
    /// panel is fed from -- the name, the root, the declared fields, the description
    /// under the picker. The picker was the one left out, and that was MOLE-224.
    ///
    /// The binding that suggests itself, `currentIndex: indexOfKind(factory,
    /// variant)`, would in fact have worked. That is measured rather than assumed,
    /// because the received wisdom is that it breaks: a `ComboBox` writes
    /// `currentIndex` through its own C++ setter when a row is activated, and an
    /// internal write of that kind does **not** remove a QML binding -- only an
    /// imperative assignment from JavaScript does. So feeding it from here is a
    /// choice to keep one way of filling this panel, not a workaround for a trap.
    ///
    /// `editText` is assigned as well, and that half is not redundant. It follows
    /// `currentIndex` by itself whenever the index really changes -- including to
    /// the empty string at -1 -- so selecting a drive needs nothing more. But
    /// assigning the index it already holds emits nothing, and `editable: true`
    /// means somebody can leave text half-typed in a picker that is already at -1;
    /// clearing the form has to clear that too.
    function showKind(factoryScheme, variantName) {
        const index = indexOfKind(factoryScheme, variantName)
        kindPicker.currentIndex = index
        kindPicker.editText = index < 0 ? "" : kindPicker.labelAt(index)
    }

    function startNew() {
        editingId = ""
        factory = ""
        variant = ""
        values = ({})
        nameField.text = ""
        rootField.text = ""
        showAdvanced = false
        showKind("", "")
    }

    function startEditing(drive) {
        editingId = drive.id
        factory = drive.factory
        variant = drive.variant
        values = Object.assign({}, drive.settings)
        nameField.text = drive.name
        rootField.text = drive.root
        showAdvanced = false
        showKind(drive.factory, drive.variant)
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

        // A line and a button, not a second copy of the explanation. The store
        // is asked for by ui/UnlockDialog.qml, which is where that copy lives:
        // one sentence to keep true rather than two that will disagree.
        Rectangle {
            objectName: "drivesLockedNote"
            Layout.fillWidth: true
            visible: App.credentialsAvailable && !App.credentialsUnlocked
            radius: 4
            color: Qt.alpha(App.colour.warn, 0.16)
            border.color: App.colour.warn
            implicitHeight: lockedRow.implicitHeight + 18

            RowLayout {
                id: lockedRow
                anchors.fill: parent
                anchors.margins: 9
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: App.colour.warn
                    font.pixelSize: App.smallTextSize
                    text: App.credentialsExist
                          ? "The credential store is shut, so a drive with a password cannot connect yet."
                          : "No credential store yet. One is made the first time you set a passphrase."
                }
                Button {
                    objectName: "drivesUnlockButton"
                    text: App.credentialsExist ? "Unlock…" : "Set a passphrase…"
                    font.pixelSize: App.secondaryTextSize
                    onClicked: App.requestCredentials()
                }
            }
        }

        // ---- what the last check found ---------------------------------------
        //
        // Its own band across the top rather than a line inside the form, because
        // saving clears the form: the answer would vanish at the exact moment it
        // became worth reading.
        Rectangle {
            objectName: "driveCheckBanner"
            Layout.fillWidth: true
            visible: dialog.checkMessage.length > 0
            radius: 4
            color: dialog.checkOk ? Qt.alpha(App.colour.ok, 0.16)
                                  : Qt.alpha(App.colour.bad, 0.16)
            border.color: dialog.checkOk ? App.colour.ok : App.colour.bad
            // Sized from an inner layout rather than straight from the wrapped
            // label. Taking the height from a label whose own height depends on
            // its width closes a binding loop, and Qt answers a loop by
            // abandoning the layout -- which leaves the band present, correct and
            // zero pixels wide. Same shape as the unlock banner above.
            implicitHeight: checkRow.implicitHeight + 16

            ColumnLayout {
                id: checkRow
                anchors.fill: parent
                anchors.margins: 8

                Label {
                    objectName: "driveCheckResult"
                    Layout.fillWidth: true
                    text: (dialog.checkOk ? "✓  " : "✕  ") + dialog.checkMessage
                    color: dialog.checkOk ? App.colour.ok : App.colour.bad
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                // Offered rather than done. Anything a sweep found is being paid
                // for, so it is worth acting on -- and it is still somebody's, so
                // it is not thrown away without being asked.
                Button {
                    objectName: "driveClearLeftoversButton"
                    visible: dialog.sweptFound > 0
                    text: "Clear " + dialog.sweptFound + (dialog.sweptFound === 1 ? " upload" : " uploads")
                    font.pixelSize: 11
                    onClicked: {
                        dialog.checkMessage = "Clearing up…"
                        App.sweepDrive(dialog.sweptDriveId, true)
                        dialog.sweptFound = 0
                    }
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
                        // Roles of the one drive model, not fields of a list
                        // built for this dialog. The state shown here and the
                        // state shown in the sidebar are now the same answer.
                        required property string configuredId
                        required property string displayName
                        required property string stateText
                        required property string stateSeverity
                        required property bool canConnect
                        required property bool canEject

                        width: ListView.view.width
                        implicitHeight: 40
                        radius: 4
                        color: configuredId === dialog.editingId ? App.colour.selection
                             : driveMouse.containsMouse ? App.colour.hover : "transparent"

                        MouseArea {
                            id: driveMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            // The configuration is fetched when a row is opened
                            // rather than carried on every row all the time.
                            onClicked: dialog.startEditing(App.driveConfiguration(configuredId))
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    text: displayName
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: stateText
                                    color: stateSeverity === "good" ? App.colour.ok
                                         : stateSeverity === "attention" ? App.colour.warn
                                         : stateSeverity === "broken" ? App.colour.bad
                                         : App.colour.textMuted
                                    font.pixelSize: 10
                                }
                            }

                            ToolButton {
                                objectName: "driveCheckButton"
                                text: "?"
                                implicitWidth: 24
                                implicitHeight: 24
                                ToolTip.text: "Check that this drive can be reached"
                                ToolTip.visible: hovered
                                // Asking is cheap and the answer is the thing a
                                // configuration cannot tell you by looking at it.
                                onClicked: {
                                    dialog.checkMessage = "Checking " + displayName + "…"
                                    dialog.checkOk = false
                                    App.checkDrive(configuredId)
                                }
                            }

                            ToolButton {
                                objectName: "driveSweepButton"
                                text: "⌫"
                                implicitWidth: 24
                                implicitHeight: 24
                                ToolTip.text: "Look for uploads this drive never finished"
                                ToolTip.visible: hovered
                                // An upload interrupted by the machine losing
                                // power is still on the server and still being
                                // charged for, and no listing will ever show it.
                                onClicked: {
                                    dialog.checkMessage = "Looking over " + displayName + "…"
                                    dialog.checkOk = true
                                    dialog.sweptFound = 0
                                    App.sweepDrive(configuredId, false)
                                }
                            }

                            ToolButton {
                                objectName: "driveConnectButton"
                                text: canEject ? "⏏" : "▶"
                                implicitWidth: 24
                                implicitHeight: 24
                                // Redundant with the sidebar, and harmless now
                                // that it is not the only way. What mattered was
                                // that it was the only way.
                                enabled: canEject || canConnect
                                onClicked: {
                                    if (canEject) {
                                        App.disconnectDrive(configuredId)
                                    } else {
                                        const problem = App.connectDrive(configuredId)
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
                        color: App.colour.textMuted
                        font.pixelSize: 12
                    }
                    Picker {
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
                    color: App.colour.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    text: {
                        const index = dialog.indexOfKind(dialog.factory, dialog.variant)
                        if (index < 0)
                            return ""
                        const kind = App.driveKinds[index]
                        return kind.available ? kind.description : kind.unavailableReason
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
                    color: App.colour.textMuted
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
                        color: App.colour.textMuted
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
                        color: App.colour.textMuted
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
                                color: modelData.required ? App.colour.textSecondary : App.colour.textMuted
                            }
                            Label {
                                visible: modelData.secret
                                text: "encrypted"
                                color: App.colour.ok
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
                            color: App.colour.textFaint
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
                        color: App.colour.bad
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
                    ActionButton {
                        objectName: "saveDriveButton"
                        text: "Save"
                        enabled: nameField.text.trim().length > 0 && dialog.factory.length > 0
                        onClicked: {
                            saveError.text = ""
                            // Saying so before the answer arrives, because the
                            // check does real network I/O and can take a moment
                            // against a host that is not answering.
                            dialog.checkOk = false
                            dialog.checkMessage = "Checking " + nameField.text.trim() + "…"
                            if (App.saveDrive(dialog.editingId, nameField.text, dialog.factory,
                                              dialog.variant, rootField.text, dialog.values)) {
                                dialog.startNew()
                            } else {
                                dialog.checkMessage = ""
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
        Picker {
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
