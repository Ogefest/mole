import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A ComboBox whose dropdown is as wide as the names in it.
//
// `implicitContentWidthPolicy: WidestText` sizes the *closed* control, and that
// part worked. The list it drops down is a separate measurement and the two did
// not agree. The style builds each row as an ItemDelegate in its own default font
// with padding of its own, and then hands the list the control's width -- so every
// row was asked to draw text a third larger than the control had measured, in less
// room than the control had, and elided it. "Identical contents" arrived in the
// list it is chosen from as "Identical c...", which is the one place the whole
// name matters.
//
// Two things fix it. A row is laid out the way the closed control is -- the same
// font and the same padding either side -- and the list is made as wide as its
// widest row rather than as wide as the control happens to be.
ComboBox {
    id: picker

    implicitContentWidthPolicy: ComboBox.WidestText

    // The list drops onto the panel ground like every other popup. Said here
    // because the window no longer hands a background down -- see ADR-0074 -- and
    // without it the style falls back to Material's own grey, which is a colour
    // nobody in this application chose.
    Material.background: App.colour.panel

    /// The label of entry `index`, however the model spells one: a `textRole` out
    /// of an object, or the entry itself when it is already a string.
    function labelAt(index) {
        const entry = model ? model[index] : undefined
        if (entry === undefined || entry === null)
            return ""
        if (textRole.length > 0 && entry[textRole] !== undefined)
            return String(entry[textRole])
        return String(entry)
    }

    /// What the widest label is expected to need, before any row exists to ask.
    ///
    /// An estimate, not the answer: the rows are not built until the list is opened
    /// for the first time, and a list that is the wrong width for one frame is a
    /// list that visibly jumps as it appears. What a row really asks for arrives
    /// below and wins, because the style is what decides how a row is laid out and
    /// it does not agree with a plain Text to the pixel.
    property real estimatedRowWidth: 0
    /// The widest any row has actually asked to be. Grows only, so opening the list
    /// twice cannot make it narrower than it was.
    property real measuredRowWidth: 0

    function remeasure() {
        const entries = picker.model
        let widest = 0
        if (entries && entries.length !== undefined) {
            for (let i = 0; i < entries.length; ++i) {
                ruler.text = picker.labelAt(i)
                widest = Math.max(widest, ruler.implicitWidth)
            }
        }
        picker.estimatedRowWidth = widest
        // The rows that measured the old labels have nothing to say about the new
        // ones, so their answer goes with them.
        picker.measuredRowWidth = 0
    }

    function noteRowWidth(width) {
        if (width > picker.measuredRowWidth)
            picker.measuredRowWidth = width
    }

    onModelChanged: remeasure()
    onFontChanged: remeasure()
    Component.onCompleted: remeasure()

    Text {
        id: ruler
        visible: false
        font: picker.font
    }

    // The list takes its width from the control, which belongs to the style rather
    // than to anything settable here -- so it is bound from outside instead. Never
    // narrower than the control: a dropdown thinner than the thing it drops from
    // reads as a fault of its own.
    Binding {
        target: picker.popup
        property: "width"
        value: Math.max(picker.width,
                        picker.estimatedRowWidth + picker.leftPadding + picker.rightPadding,
                        picker.measuredRowWidth)
        when: picker.estimatedRowWidth > 0 || picker.measuredRowWidth > 0
        restoreMode: Binding.RestoreNone
    }

    delegate: ItemDelegate {
        required property int index

        width: ListView.view.width
        // The control's font, not the style's default for a list row -- a row that
        // measured its text a third wider than the control did was half the fault.
        font: picker.font
        // And its padding. The right side is where the indicator sits on the closed
        // control, so a row is left with an empty column there; that costs a few
        // pixels of white space and buys a list that cannot cut a name in half.
        leftPadding: picker.leftPadding
        rightPadding: picker.rightPadding
        highlighted: picker.highlightedIndex === index
        text: picker.labelAt(index)

        // What this row would need to draw its label whole. Reported rather than
        // assumed, because implicitWidth is the style's answer and no measurement
        // taken outside a row reproduces it exactly.
        onImplicitWidthChanged: picker.noteRowWidth(implicitWidth)
        Component.onCompleted: picker.noteRowWidth(implicitWidth)
    }
}
