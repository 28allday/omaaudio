import QtQuick

// Named actions resolved through a keymap file (BRIEF.md section 6).
//
// The shipped defaults are compiled into the binary; a user file at
// $XDG_CONFIG_HOME/omawave/keymap.toml overrides individual entries. No key is
// hardcoded anywhere else -- a Keys handler asks this for an action name and never
// tests for a specific key.
Item {
    id: root

    // action name -> binding string, e.g. { play_stop: "Space" }
    property var bindings: ({})

    signal changed()

    // Both files come through the Backend: the defaults out of the binary's resources,
    // the overrides off disk. QML reads no files of its own.
    Component.onCompleted: root._rebuild()

    Connections {
        target: backend
        // Re-read when the user's file changes, so a rebind takes effect without a
        // restart.
        function onKeymapChanged() { root._rebuild() }
    }

    // Parses the same strict TOML subset the IPC protocol uses: key = "value", one per
    // line, comments starting with #. Unquoted values are accepted here because a
    // human writes this file by hand.
    function _parse(text) {
        var out = {}
        if (!text)
            return out
        var lines = String(text).split("\n")
        for (var i = 0; i < lines.length; ++i) {
            var line = lines[i]
            var hash = line.indexOf("#")
            if (hash >= 0)
                line = line.substring(0, hash)
            line = line.trim()
            if (line === "")
                continue
            var eq = line.indexOf("=")
            if (eq < 0)
                continue
            var key = line.substring(0, eq).trim()
            var value = line.substring(eq + 1).trim()
            if (value.length >= 2 && value[0] === '"' && value[value.length - 1] === '"')
                value = value.substring(1, value.length - 1)
            if (key !== "" && value !== "")
                out[key] = value
        }
        return out
    }

    // True when the user's file overrides at least one binding; the status bar
    // says which keymap is in force.
    property bool userOverrides: false

    function _rebuild() {
        var merged = root._parse(backend.defaultKeymap())
        var user = root._parse(backend.userKeymap())
        // Per-entry override: a user file naming one action keeps every other default.
        var overrides = false
        for (var k in user) {
            merged[k] = user[k]
            overrides = true
        }
        root.userOverrides = overrides
        root.bindings = merged
        root.changed()
    }

    // Builds the canonical binding string for a key event, so lookups match the file.
    function _describe(event) {
        var parts = root._modifierPrefix(event.modifiers)
        var name = root._keyName(event.key)
        if (name === "")
            return ""
        parts.push(name)
        return parts.join("+")
    }

    function _keyName(key) {
        switch (key) {
        case Qt.Key_Space: return "Space"
        case Qt.Key_Home: return "Home"
        case Qt.Key_End: return "End"
        case Qt.Key_Escape: return "Escape"
        case Qt.Key_Left: return "Left"
        case Qt.Key_Right: return "Right"
        case Qt.Key_Up: return "Up"
        case Qt.Key_Down: return "Down"
        case Qt.Key_Delete: return "Delete"
        case Qt.Key_Backspace: return "Backspace"
        case Qt.Key_Return: return "Return"
        case Qt.Key_Enter: return "Return"
        case Qt.Key_BracketLeft: return "["
        case Qt.Key_BracketRight: return "]"
        case Qt.Key_Plus: return "+"
        case Qt.Key_Minus: return "-"
        case Qt.Key_Equal: return "="
        case Qt.Key_Comma: return ","
        case Qt.Key_Period: return "."
        case Qt.Key_QuoteLeft: return "`"
        }
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return "F" + (key - Qt.Key_F1 + 1)
        if (key >= Qt.Key_A && key <= Qt.Key_Z)
            return String.fromCharCode("A".charCodeAt(0) + (key - Qt.Key_A))
        if (key >= Qt.Key_0 && key <= Qt.Key_9)
            return String.fromCharCode("0".charCodeAt(0) + (key - Qt.Key_0))
        return ""
    }

    function _modifierPrefix(modifiers) {
        var parts = []
        if (modifiers & Qt.ControlModifier) parts.push("Ctrl")
        if (modifiers & Qt.ShiftModifier) parts.push("Shift")
        if (modifiers & Qt.AltModifier) parts.push("Alt")
        if (modifiers & Qt.MetaModifier) parts.push("Meta")
        return parts
    }

    // Wheel gestures are bindings too, so they resolve through the same file rather
    // than being hardcoded (BRIEF.md section 6). A binding of "Ctrl+Scroll" matches a
    // wheel event with Ctrl held; plain "Scroll" matches with no modifiers.
    function actionForWheel(modifiers) {
        var combo = root._modifierPrefix(modifiers).concat(["Scroll"]).join("+")
        for (var action in root.bindings) {
            if (root.bindings[action] === combo)
                return action
        }
        return ""
    }

    // Returns the action bound to this event, or "" if none is. A key may serve
    // a Spectral tool and an action elsewhere (L, O): on the Spectral page its
    // spectral_ action wins, on the others the other one.
    function actionFor(event, workspace) {
        var combo = root._describe(event)
        if (combo === "")
            return ""
        var found = ""
        for (var action in root.bindings) {
            if (root.bindings[action] !== combo)
                continue
            var spectral = action.indexOf("spectral_") === 0
            if (spectral === (workspace === "Spectral"))
                return action
            if (found === "")
                found = action
        }
        return found
    }
}
