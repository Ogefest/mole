import QtQuick

// Hands window shortcuts back to the window when a text editor has stolen them.
//
// A read-only `TextArea` is a viewer, but Qt still treats it as an editor for
// shortcut purposes: it accepts the shortcut-override event for every key in
// the standard editing bindings, which tells Qt not to run the matching
// `Shortcut`. `Ctrl+W` is `DeleteStartOfWord`, so clicking into a preview
// silently stopped `Ctrl+W` from closing the tab — the key reached the text
// control, which discarded it because the document is read-only.
//
// There is no declarative way to un-claim those keys, so the affected view
// forwards them here instead of each one growing its own private copy of the
// shortcut table. Attach it as
//
//     TextArea { Keys.onPressed: (event) => viewerKeys.relay(event) }
//     ViewerKeys { id: viewerKeys }
//
// Deliberately narrow: only keys a viewer has no use for. Anything the viewer
// does use -- Ctrl+C, Ctrl+A, Ctrl+Home -- is left for it to handle.
QtObject {
    id: relayer

    /// Keys the surrounding view claims for itself, as `[key, modifiers]` pairs.
    /// A preview pages with Ctrl+PgUp/PgDn, so those must not be forwarded.
    property var reserved: []

    function isReserved(event) {
        for (var i = 0; i < reserved.length; ++i) {
            if (reserved[i][0] === event.key && reserved[i][1] === event.modifiers)
                return true
        }
        return false
    }

    /// Returns true when the key was handled, in which case the caller should
    /// mark the event accepted so the text control never sees it.
    ///
    /// **One table, and it is in C++.** This was a switch over Qt.Key_* here --
    /// a second copy of the window's key table, which already disagreed with
    /// Main.qml about nine keys: Ctrl+R, Ctrl+Shift+I/S/C/F, Ctrl+G/L, Ctrl+`,
    /// Ctrl+Q and Ctrl+PgUp/PgDn were all swallowed by a viewer and never handed
    /// back. `App.relayWindowKey()` is the one list now, beside the actions it
    /// triggers; what it deliberately leaves alone is written down there. See
    /// MOLE-396 and ADR-0002.
    function relay(event) {
        if ((event.modifiers & Qt.ControlModifier) === 0 || isReserved(event))
            return false
        if (!App.relayWindowKey(event.key, event.modifiers))
            return false
        event.accepted = true
        return true
    }
}
