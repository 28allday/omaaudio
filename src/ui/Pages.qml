pragma Singleton
import QtQuick

// The words shown for the four pages. Code, the keymap, saved layouts and the
// command line keep the page names Waveform, Multitrack, Spectral and Mastering.
QtObject {
    function label(name) {
        return name === "Waveform" ? qsTr("Take")
             : name === "Multitrack" ? qsTr("Arrange")
             : name === "Spectral" ? qsTr("Spectral")
             : name === "Mastering" ? qsTr("Mastering") : name
    }
}
