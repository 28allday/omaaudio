pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Audio Setup: what the audio system looks like from here, in words a person
// can act on. Read-only except for one thing done on request: routing
// OmaAudio's playback to a JACK client. It never changes a system default,
// opens a device or starts monitoring; the desktop's sound settings own those.
C.Popup {
    id: root
    objectName: "audioSetupPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var picture: service ? service.audioSetup : ({})
    readonly property bool scanning: service ? service.audioSetupScanning : false
    readonly property bool scanned: picture && picture.scannedAt !== undefined

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    height: Math.min(640, parent ? parent.height - 32 : 640)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: { if (service) service.refreshAudioSetup(); refreshButton.forceActiveFocus(Qt.TabFocusReason) }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    function rate(hz) { return hz > 0 ? (hz % 1000 === 0 ? (hz / 1000) + " kHz" : (hz / 1000).toFixed(1) + " kHz") : qsTr("unknown") }
    function stateWord(s) {
        if (s === "running") return qsTr("running")
        if (s === "idle") return qsTr("idle")
        if (s === "suspended") return qsTr("suspended, wakes on use")
        if (s === "error") return qsTr("error")
        return s ? s : qsTr("unknown")
    }

    component Heading: Text {
        Layout.fillWidth: true; Layout.topMargin: Theme.s2
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
        color: Theme.textMuted
    }
    component Line: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; color: Theme.textPrimary
    }
    component Note: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Audio Setup"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                id: refreshButton
                objectName: "audioSetupRefresh"
                iconName: "refresh-cw"; text: root.scanning ? qsTr("Cancel reading") : qsTr("Refresh"); showLabel: true
                enabled: root.service !== null
                unavailableReason: qsTr("The audio system is being read.")
                helpText: root.scanning ? qsTr("Stop waiting for the audio system. Refresh to try again.") : qsTr("Read the audio system again: devices, rates, connections and JACK clients.")
                helpScope: qsTr("Looks only. Nothing is changed by refreshing.")
                onClicked: root.scanning ? root.service.cancelAudioSetupScan() : root.service.refreshAudioSetup()
            }
            ToolButton {
                objectName: "audioSetupClose"
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close Audio Setup.")
            }
        }
        Note {
            text: qsTr("What PipeWire and the kernel report right now. OmaAudio never changes a system default, opens a device on its own or starts monitoring; choose defaults and rates in the desktop's sound settings.")
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.service !== null && root.service.audioLinkMessage !== ""
            Note { objectName: "audioLinkStatus"; text: root.service ? root.service.audioLinkMessage : "" }
            ToolButton {
                objectName: "audioLinkCancel"; text: qsTr("Cancel connection"); showLabel: true
                visible: root.service !== null && root.service.audioLinkBusy
                helpText: qsTr("Stop the remaining connection changes and refresh the audio system.")
                helpScope: qsTr("Channels already changed stay connected or disconnected as shown after the refresh.")
                onClicked: root.service.cancelAudioLinks()
            }
        }
        Note {
            objectName: "audioSetupProblem"
            visible: !!(root.picture && root.picture.problem)
            color: Theme.warning
            text: root.picture && root.picture.problem ? root.picture.problem : ""
        }

        ScrollPane {
            id: setupScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
            ColumnLayout {
                x: setupScroll.reachLeft; width: setupScroll.columnWidth
                spacing: Theme.s2

                Heading { text: qsTr("PLAYBACK") }
                Line {
                    objectName: "audioSetupOutput"
                    text: {
                        if (!root.scanned) return root.scanning ? qsTr("Reading the audio system…") : qsTr("Press Refresh to read the audio system.")
                        const o = root.picture.output
                        if (!o || !o.present) return qsTr("OmaAudio's playback stream is not in the graph. The audio engine may still be starting, or PipeWire is not running.")
                        const to = o.connectedTo && o.connectedTo.length ? o.connectedTo.join(", ") : qsTr("nothing yet (it connects when playback starts)")
                        return qsTr("OmaAudio's playback stream is %1, at %2, %3 channels, connected to: %4.")
                            .arg(root.stateWord(o.state)).arg(root.rate(o.rate)).arg(o.channels).arg(to)
                    }
                }
                Line {
                    objectName: "audioSetupRates"
                    visible: root.scanned
                    text: {
                        const s = root.picture.settings || {}
                        const project = root.picture.projectRate || 0
                        const graph = root.picture.graphRate || s.clockRate || 0
                        var text = qsTr("Project rate %1; device rate %2; audio buffer %3 samples")
                            .arg(root.rate(project)).arg(root.rate(s.clockRate)).arg(s.quantum || qsTr("unknown"))
                        if (s.forceRate > 0) text += qsTr(" (forced to %1)").arg(root.rate(s.forceRate))
                        if (s.allowedRates && s.allowedRates.length > 1) text += qsTr("; allowed rates %1").arg(s.allowedRates.map(root.rate).join(", "))
                        text += "."
                        if (project > 0 && graph > 0 && project !== graph)
                            text += " " + qsTr("The project and the system clock differ: PipeWire converts the output. Match them in Project Properties or the desktop's sound settings if you prefer.")
                        return text
                    }
                }

                Heading { text: qsTr("OUTPUTS") }
                Repeater {
                    model: root.scanned ? root.picture.sinks : []
                    delegate: Line {
                        required property var modelData
                        text: (modelData.isDefault ? "● " : "○ ") + modelData.description
                              + qsTr(" — %1 channels, %2").arg(modelData.channels).arg(root.stateWord(modelData.state))
                              + (modelData.isDefault ? qsTr(" (system default)") : "")
                    }
                }
                Note { visible: root.scanned && (!root.picture.sinks || root.picture.sinks.length === 0); text: qsTr("No output device is available to PipeWire.") }

                Heading { text: qsTr("INPUTS") }
                Repeater {
                    model: root.scanned ? root.picture.sources : []
                    delegate: Line {
                        required property var modelData
                        text: (modelData.isDefault ? "● " : "○ ") + modelData.description
                              + qsTr(" — %1 channels, %2").arg(modelData.channels).arg(root.stateWord(modelData.state))
                              + (modelData.isDefault ? qsTr(" (system default)") : "")
                    }
                }
                Note { visible: root.scanned && (!root.picture.sources || root.picture.sources.length === 0); text: qsTr("No input device is available to PipeWire. The Recording panel can still use a JACK client's output.") }
                Note { text: qsTr("Choose the input for a recording in the Recording panel. The system default is what the desktop's sound settings select.") }

                Heading { text: qsTr("SOUND CARDS (ALSA)") }
                Note { text: qsTr("What the kernel has detected. A card is usable only when PipeWire exposes an output or input for it; a detected card without one is typically disabled in the desktop's sound settings, in use by another program, or unsupported.") }
                Repeater {
                    model: root.scanned ? root.picture.alsaCards : []
                    delegate: Line {
                        required property var modelData
                        objectName: "audioSetupCard"
                        text: {
                            var pcm = []
                            for (var i = 0; i < modelData.pcms.length; ++i) {
                                const p = modelData.pcms[i]
                                pcm.push(p.name + (p.playback && p.capture ? qsTr(" (play, capture)") : p.playback ? qsTr(" (play)") : qsTr(" (capture)")))
                            }
                            return qsTr("Card %1 · %2 — %3").arg(modelData.index).arg(modelData.name)
                                   .arg(modelData.usable ? qsTr("usable through PipeWire") : modelData.deviceId >= 0 ? qsTr("known to PipeWire, no endpoint") : qsTr("detected only"))
                                   + (pcm.length ? "\n    " + pcm.join(", ") : "")
                        }
                    }
                }
                Note { visible: root.scanned && (!root.picture.alsaCards || root.picture.alsaCards.length === 0); text: qsTr("The kernel lists no sound cards.") }

                Heading { text: qsTr("JACK") }
                Line {
                    objectName: "audioSetupJack"
                    text: {
                        if (!root.scanned) return ""
                        const j = root.picture.jack || {}
                        if (j.available)
                            return qsTr("JACK programs run through PipeWire's compatibility layer (pipewire-jack), so they appear here as clients and can be connected to OmaAudio. This is audio routing only: OmaAudio does not follow or drive JACK transport.")
                        if (j.libjack === "other")
                            return qsTr("A JACK library is installed that is not PipeWire's. JACK programs will talk to a separate JACK server, which OmaAudio does not use. Install pipewire-jack to route them here.")
                        return qsTr("pipewire-jack is not installed. JACK programs cannot connect to OmaAudio until it is.")
                    }
                }
                Repeater {
                    model: root.scanned ? root.picture.jackClients : []
                    delegate: RowLayout {
                        id: clientRow
                        required property var modelData
                        Layout.fillWidth: true
                        Line {
                            Layout.fillWidth: true
                            text: clientRow.modelData.application
                                  + qsTr(" — %1 inputs, %2 outputs, %3").arg(clientRow.modelData.inputPorts.length).arg(clientRow.modelData.outputPorts.length).arg(root.stateWord(clientRow.modelData.state))
                                  + (clientRow.modelData.connectedToOurOutput ? qsTr("; receives OmaAudio's playback") : "")
                                  + (clientRow.modelData.connectedToOurCapture ? qsTr("; feeds OmaAudio's recording") : "")
                        }
                        ToolButton {
                            objectName: "jackLink_" + clientRow.modelData.id
                            visible: clientRow.modelData.inputPorts.length > 0
                            text: clientRow.modelData.connectedToOurOutput ? qsTr("Disconnect playback") : qsTr("Send playback here"); showLabel: true
                            helpText: qsTr("Connect OmaAudio's playback ports to this client's inputs, in order, or remove those links.")
                            helpScope: qsTr("Only this client's links change. Playback to the system output continues unless you disconnect it elsewhere.")
                            enabled: root.service !== null && !root.service.audioLinkBusy && !root.scanning
                            unavailableReason: qsTr("Wait for the current connection or refresh to finish, or cancel it.")
                            onClicked: {
                                if (clientRow.modelData.connectedToOurOutput) root.service.unlinkOutputFrom(clientRow.modelData.id)
                                else root.service.linkOutputTo(clientRow.modelData.id)
                            }
                        }
                    }
                }
                Note { visible: root.scanned && root.picture.jack && root.picture.jack.available && (!root.picture.jackClients || root.picture.jackClients.length === 0); text: qsTr("No JACK client is running. Start one and press Refresh.") }
                Note { text: qsTr("A JACK client's outputs are also offered as recording inputs in the Recording panel.") }

                Heading { text: qsTr("IF THERE IS NO SOUND") }
                Note { text: qsTr("1. Check the playback line above: the stream should be running and connected to the output you expect. 2. Make that output the system default in the desktop's sound settings, then play again. 3. If the stream is missing, PipeWire may not be running: `systemctl --user status pipewire`. 4. A card listed as detected only is not available to PipeWire; look at the desktop's sound settings for a profile switched off. 5. Recording: choose the input in the Recording panel; a JACK client must be running to appear.") }
                Note { visible: root.scanned; text: qsTr("Read at %1. %2 nodes in the graph.").arg(root.picture.scannedAt).arg(root.picture.nodeCount) }
            }
        }
    }
}
