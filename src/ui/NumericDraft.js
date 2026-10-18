.pragma library

// Buttons deliberately leave keyboard focus with the editor. When a numeric
// field is being typed into, finish that draft before the button uses it.
function commit(window) {
    const input = window ? window.activeFocusItem : null
    if (input && typeof input.commitNumericDraft === "function") input.commitNumericDraft()
}

// What a validated numeric field holds, as a number. The validators accept
// what their locale writes: digit grouping ("8,192"), and a decimal comma where
// the field follows the system locale. JavaScript's Number() reads neither and
// gives NaN, which an int property or a frame argument silently turns into 0.
// Pass "C" for a field whose validator is pinned to the C locale; leave it out
// for one that follows the system. NaN only for text no validator would accept.
function parse(text, localeName) {
    const source = String(text).trim()
    if (source === "") return NaN
    try {
        return Number.fromLocaleString(localeName === undefined ? Qt.locale() : Qt.locale(localeName), source)
    } catch (e) {
        return Number(source)
    }
}
