pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "ui"

// "Check this computer": measures mixing at the current rate and buffer size and
// says, in plain words, how many tracks of each kind fit. Effects come on top.
ColumnLayout {
    id: root
    property var service: null
    readonly property var result: service ? service.computerCheck : ({})
    readonly property bool checking: !!service && service.checkingComputer
    spacing: Theme.s2

    function fits(count) {
        if (!root.service) return String(count)
        return count >= root.service.maxTracks ? qsTr("all %1").arg(root.service.maxTracks) : qsTr("about %1").arg(count)
    }

    Text {
        objectName: "computerCheckResult"
        Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        text: root.checking ? qsTr("Measuring how fast this computer mixes… (a few seconds)")
            : !root.result.ok ? (root.result.reason || qsTr("Not checked yet. The check mixes generated sessions for a few seconds; playback is not affected."))
            : qsTr("At %1 kHz with %2-sample buffers (%3 ms per block), mixing alone fits %4 plain tracks, %5 tracks with dense automation, "
                   + "or %6 tracks routed through a bus with automation, using half of each block. Effects use the rest, so heavy plug-ins lower these numbers; "
                   + "watch Audio load while playing to see the real figure.")
                .arg((root.result.sampleRate / 1000).toFixed(1)).arg(root.result.blockFrames).arg(root.result.deadlineMs.toFixed(1))
                .arg(root.fits(root.result.plainTracks)).arg(root.fits(root.result.automatedTracks)).arg(root.fits(root.result.busTracks))
    }
    ToolButton {
        objectName: "computerCheckRun"
        text: root.result.ok ? qsTr("Check again") : qsTr("Check this computer"); showLabel: true
        enabled: !!root.service && !root.checking
        onClicked: root.service.checkComputer()
        helpText: qsTr("Measure how many tracks this computer can mix at the current sample rate and buffer size.")
        helpScope: qsTr("Uses generated audio for a few seconds; your project and playback are not changed. A larger buffer in Audio Setup fits more tracks but adds delay.")
        unavailableReason: qsTr("The check is already running.")
    }
}
