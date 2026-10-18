pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The mixer: one channel strip per track and a master strip, in the brief's
// anatomy -- name and role colour, inserts, pan, mute / solo, a long fader with
// its dB readout, and a peak meter with a held peak and a sticky clip.
//
// Every control here is the same control the lane header carries, reached
// from the other side: the fader moves the same gain, the M lights the same
// mute. Nothing lives only in the mixer.
Rectangle {
    id: root
    objectName: "mixerDock"

    property var shell: null
    readonly property var service: shell ? shell.service : null

    color: Theme.panelBg

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.border
        z: 2
    }

    DockHeader {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        tabs: [qsTr("Mixer")]
        currentIndex: 0
        trailing: [
            IconButton {
                iconName: "panel-right"; text: qsTr("Hide Mixer")
                helpText: qsTr("Hide the mixer panel. Levels, pans, mutes and solos keep applying.")
                helpScope: qsTr("The same controls stay available in each lane header. Show the mixer again from the View menu.")
                shortcut: root.shell ? root.shell.bindingFor("show_mixer") : ""
                onClicked: root.shell.runAction("show_mixer")
            }
        ]
    }

    // dB <-> fader fraction, the same law as the lane's gain slider: -60..+12.
    function dbToFraction(db) { return Math.max(0, Math.min(1, (db + 60) / 72)) }
    function fractionToDb(f) { return f * 72 - 60 }

    // Stacking. Strips sit in one row while they fit across; past that they
    // wrap into as many rows as the panel's height holds at the standard fader
    // length, so a tall mixer shows more channels rather than longer faders.
    // Only what still does not fit scrolls sideways.
    readonly property int stripCount: service ? service.trackCount : 0
    property real measuredStripHeight: 0   // the first strip's minimumHeight, once laid out
    readonly property real stripMinHeight: measuredStripHeight > 0 ? measuredStripHeight : 340
    readonly property int fitAcross: Math.max(1, Math.floor(stripArea.width / Theme.wStrip))
    readonly property int fitDown: Math.max(1, Math.floor(stripArea.height / Math.max(1, stripMinHeight)))
    readonly property int stripRows: stripCount === 0 ? 1
                                     : Math.max(1, Math.min(fitDown, Math.ceil(stripCount / fitAcross)))
    readonly property int stripsPerRow: Math.max(1, Math.ceil(stripCount / stripRows))
    readonly property real stripHeight: Math.max(stripMinHeight,
        Math.floor(stripArea.height / Math.max(1, Math.ceil(stripCount / stripsPerRow))))

    // The room left of the master strip. The visible window onto the strips is a
    // whole number of strips wide and centred in it, so no strip is ever cut at
    // either edge; the leftover is equal space either side.
    Item {
        id: stripArea
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: master.left
        anchors.bottom: parent.bottom
    }

    Flickable {
        id: stripScroll
        objectName: "mixerScroll"
        anchors.top: stripArea.top
        anchors.bottom: stripArea.bottom
        anchors.horizontalCenter: stripArea.horizontalCenter
        width: Math.min(root.stripsPerRow, root.fitAcross) * Theme.wStrip
        contentWidth: strips.implicitWidth
        contentHeight: Math.max(height, strips.implicitHeight)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick

        // Scrolling moves by whole strips and always comes to rest on a strip
        // edge -- a drag snaps when it ends.
        function settle(x) {
            contentX = Math.max(0, Math.min(Math.round(x / Theme.wStrip) * Theme.wStrip,
                                            Math.max(0, contentWidth - width)))
        }
        onMovementEnded: settle(contentX)
        onWidthChanged: settle(contentX)

        // The wheel moves along the strips; without this only a drag reached the
        // strips past the right edge. A notch is one strip; small touchpad
        // movements add up until they make one.
        property real wheelRest: 0
        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                const d = event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
                    ? (Math.abs(event.pixelDelta.x) > Math.abs(event.pixelDelta.y) ? event.pixelDelta.x : event.pixelDelta.y)
                    : (Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y) ? event.angleDelta.x : event.angleDelta.y) / 120 * Theme.wStrip
                stripScroll.wheelRest += d
                const steps = stripScroll.wheelRest > 0 ? Math.floor(stripScroll.wheelRest / Theme.wStrip)
                                                        : Math.ceil(stripScroll.wheelRest / Theme.wStrip)
                stripScroll.wheelRest -= steps * Theme.wStrip
                if (steps !== 0)
                    stripScroll.settle(stripScroll.contentX - steps * Theme.wStrip)
                event.accepted = true
            }
        }

        Grid {
            id: strips
            objectName: "mixerStrips"
            columns: root.stripsPerRow
            spacing: 0

            Repeater {
                id: stripRepeater
                model: root.service ? root.service.trackIdModel : null

                delegate: Strip {
                    id: stripDelegate
                    required property var modelData
                    required property int index
                    Binding {
                        when: stripDelegate.index === 0
                        target: root
                        property: "measuredStripHeight"
                        value: stripDelegate.minimumHeight
                    }
                    property int revision: 0   // this track's data changed (see MultitrackWorkspace)
                    Connections {
                        target: root.service
                        function onTrackDataChanged(ids) { if (ids.indexOf(stripDelegate.modelData) >= 0) ++stripDelegate.revision }
                    }
                    track: {
                        stripDelegate.revision
                        const candidate = root.service ? root.service.trackById(modelData) : null
                        return candidate && candidate.id !== undefined ? candidate : null
                    }
                    height: root.stripHeight
                }
            }
        }
    }

    // The strips carry their own right-hand rule; this is the left edge of the window.
    Rectangle {
        x: stripScroll.x - Theme.hairline
        anchors.top: stripArea.top
        anchors.bottom: stripArea.bottom
        width: Theme.hairline
        color: Theme.border
        visible: root.stripCount > 0
    }

    // The master strip. Its fader is the monitor level, which is NOT
    // rendered into a mixdown -- the readout says so. Pinned at the right
    // edge, full height, so it stays in reach however the strips stack.
    Rectangle {
        id: master
        objectName: "masterStrip"
        anchors.top: header.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: visible ? Theme.wStrip + Theme.s2 : 0
        color: Theme.panelRaised
        visible: root.service !== null && root.service.trackCount > 0

        // Right-click anywhere on the master strip: the master bus's menu.
        MouseArea {
            objectName: "masterStripMenuMouse"
            anchors.fill: parent
            z: 10
            acceptedButtons: Qt.RightButton
            onPressed: if (root.shell) root.shell.masterContextMenu.popup()
        }

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.hairline
            color: Theme.borderStrong
        }

        Column {
            anchors.fill: parent
            anchors.leftMargin: Theme.s1 + Theme.hairline
            anchors.rightMargin: Theme.s1
            anchors.topMargin: Theme.s1
            anchors.bottomMargin: Theme.s1
            spacing: Theme.s1

            StripHeader {
                width: parent.width
                tint: Theme.roleColor("master")
                name: qsTr("Master")
                role: qsTr("monitor")
            }

            // The master bus's effects: a count that shows them in the Effects
            // panel, as a track strip's does for its track.
            Rectangle {
                objectName: "masterStripFx"
                width: parent.width
                height: Theme.hRow
                radius: Theme.rControl
                color: masterFxHover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
                border.width: Theme.hairline
                border.color: root.shell && root.shell.effectsDockShowsMaster ? Theme.accent : Theme.border
                readonly property int count: root.service ? root.service.masterInserts.length : 0
                Row {
                    anchors.centerIn: parent
                    spacing: Theme.s1
                    Icon { name: "sparkles"; size: 12; color: parent.parent.count > 0 ? Theme.accent : Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: parent.parent.count === 0 ? qsTr("no FX") : qsTr("%1 FX").arg(parent.parent.count)
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: parent.parent.count > 0 ? Theme.textPrimary : Theme.textMuted
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                HoverHandler { id: masterFxHover }
                TapHandler {
                    onTapped: {
                        if (!root.shell || !root.service) return
                        root.shell.showMasterEffects()
                        // One effect: its controls straight away.
                        if (parent.count === 1)
                            root.shell.openEffectPanel(root.service.masterTarget, 0)
                    }
                }
                Accessible.role: Accessible.Button
                Accessible.name: masterFxHelp.title
                Accessible.description: masterFxHelp.description
                ControlHelp {
                    id: masterFxHelp
                    title: qsTr("Master effects")
                    purpose: qsTr("Show the master bus's effects in the Effects panel; with one effect, its controls open too. Right-click the strip to add one.")
                    scope: qsTr("The master bus processes the whole mix in playback and exports.")
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Listening level. Not in a mixdown.")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsRuler
                color: Theme.textMuted
            }

            Item { width: 1; height: Theme.s2 }

            FaderAndMeter {
                objectName: "masterFader"
                helpTitle: qsTr("Listening level")
                helpText: qsTr("Set how loud you hear the mix, in decibels (dB). This is the monitor level: it is not exported into an export.")
                helpScope: qsTr("Range −60 to +12 dB; double-click returns to 0 dB. To change the level that is exported, use the master bus input trim in the Track Effects panel.")
                meterTitle: qsTr("Master peak meter")
                width: parent.width
                height: Math.max(implicitHeight, parent.height - y)
                db: root.service ? root.service.masterGainDb : 0
                peakLeft: root.service ? root.service.peakLeft : 0
                peakRight: root.service ? root.service.peakRight : 0
                playing: root.service ? root.service.playing : false
                onMoved: function (value) { if (root.service) root.service.setGainDb(value) }
                onReset: if (root.service) root.service.setGainDb(0)
            }
        }
    }

    EmptyState {
        anchors.fill: parent
        anchors.topMargin: header.height
        visible: root.service === null || root.service.trackCount === 0
        iconName: "sliders-vertical"
        title: qsTr("No tracks to mix")
        description: qsTr("Every track gets a channel strip here.")
    }

    // ── one channel strip ────────────────────────────────────────────────
    component Strip: Rectangle {
        id: strip
        objectName: "mixerStrip"

        property var track: null
        readonly property int trackId: track ? track.id : -1
        readonly property bool focused: root.service !== null && track !== null
                                        && root.service.focusedTrack === track.id
        readonly property color tint: track && track.color !== "" ? track.color
                                    : Theme.roleColor(track ? track.role : "dialogue")
        readonly property var meter: (root.service && root.service.meterRevision >= 0 && strip.trackId >= 0)
                                     ? root.service.trackMeter(strip.trackId) : null

        width: Theme.wStrip
        color: focused ? Theme.panelRaised : Theme.panelBg
        // The height at which the fader is its standard length: what one row of
        // a stacked mixer must give each strip.
        readonly property real minimumHeight: stripFaderItem.y + stripFaderItem.implicitHeight + Theme.s1 * 2

        // Rows of a stacked mixer are ruled apart.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Theme.hairline
            color: Theme.border
            visible: root.stripRows > 1
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.hairline
            color: Theme.border
        }

        // Clicking anywhere on the strip that is not a control focuses the track.
        MouseArea {
            anchors.fill: parent
            z: -1
            onClicked: if (root.service && strip.track) root.service.focusTrack(strip.track.id)
            ControlHelp {
                title: qsTr("Channel strip")
                purpose: qsTr("Click an empty part of the strip to focus this track: the one keys, the rack and the inspector act on.")
            }
        }

        // Right-click anywhere on the strip: the track menu, the same one the
        // lane opens (rename, duplicate, remove…), for this strip's track.
        MouseArea {
            objectName: "stripMenuMouse"
            anchors.fill: parent
            z: 10
            acceptedButtons: Qt.RightButton
            onPressed: {
                if (!root.service || !strip.track || !root.shell) return
                root.service.focusSelectedTrack(strip.track.id)
                root.shell.laneContextMenu.popup()
            }
        }

        Column {
            anchors.fill: parent
            anchors.leftMargin: Theme.s1
            anchors.rightMargin: Theme.s1 + Theme.hairline
            anchors.topMargin: Theme.s1
            anchors.bottomMargin: Theme.s1
            spacing: Theme.s1

            StripHeader {
                objectName: "stripHeader"
                width: parent.width
                tint: strip.tint
                name: strip.track ? strip.track.name : ""
                role: strip.track ? Theme.roleLabel(strip.track.role) : ""
                lit: strip.focused
                onClicked: if (root.service && strip.track) root.service.focusTrack(strip.track.id)
            }

            // Inserts: a count that opens the rack on this track.
            Rectangle {
                objectName: "stripFx"
                width: parent.width
                height: Theme.hRow
                radius: Theme.rControl
                color: fxHover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
                border.width: Theme.hairline
                border.color: Theme.border
                readonly property int count: strip.track && strip.track.inserts ? strip.track.inserts.length : 0
                Row {
                    anchors.centerIn: parent
                    spacing: Theme.s1
                    Icon { name: "sparkles"; size: 12; color: parent.parent.count > 0 ? Theme.accent : Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: parent.parent.count === 0 ? qsTr("no FX") : qsTr("%1 FX").arg(parent.parent.count)
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: parent.parent.count > 0 ? Theme.textPrimary : Theme.textMuted
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                HoverHandler { id: fxHover }
                TapHandler {
                    onTapped: {
                        if (root.service && strip.track)
                            root.service.focusTrack(strip.track.id)
                        if (root.shell) {
                            root.shell.showTrackEffects()
                            root.shell.effectsDockVisible = true
                        }
                        // One effect: its controls straight away.
                        if (root.shell && strip.track && parent.count === 1)
                            root.shell.openEffectPanel(strip.track.id, 0)
                    }
                }
                Accessible.role: Accessible.Button
                Accessible.name: fxHelp.title
                Accessible.description: fxHelp.description
                ControlHelp {
                    id: fxHelp
                    title: qsTr("Effects")
                    purpose: qsTr("Open this track's effects in the Track Effects panel; with one effect, its controls open too. The count is how many effects the track has.")
                    scope: qsTr("Also focuses the track. Effects run live on playback and in exports.")
                }
            }

            // Pan, drawn from the centre.
            Slider {
                objectName: "stripPan"
                width: parent.width
                label: qsTr("Pan")
                value: strip.track ? (strip.track.pan + 1) / 2 : 0.5
                centred: true
                onMoved: function (fraction) {
                    if (root.service && strip.track)
                        root.service.setTrackPan(strip.track.id, fraction * 2 - 1)
                }
                onReset: if (root.service && strip.track) root.service.setTrackPan(strip.track.id, 0)
                onTouched: pressed => {
                    panHelp.suppressed = pressed
                    if (root.service && strip.track) root.service.automationGesture(strip.track.id, "pan", pressed)
                }
                Accessible.role: Accessible.Slider
                Accessible.name: panHelp.title
                Accessible.description: panHelp.description
                ControlHelp {
                    id: panHelp
                    title: qsTr("Pan")
                    purpose: qsTr("Place this track between left and right. Drag the handle; double-click returns it to the centre.")
                    scope: qsTr("Applies to this track's output in playback and exports. If pan automation is enabled on the track, the curve overrides this control.")
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.s1
                StateButton {
                    objectName: "stripMute"
                    label: "M"; tip: ""
                    active: strip.track ? strip.track.muted === true : false
                    activeColor: Theme.warning
                    onClicked: if (root.service && strip.track) root.service.toggleMute(strip.track.id)
                    ControlHelp {
                        title: qsTr("Mute")
                        purpose: qsTr("Silence this track. Mute also silences its sends.")
                        scope: qsTr("Acts on this track only. A soloed track elsewhere silences this one as well.")
                    }
                }
                StateButton {
                    objectName: "stripSolo"
                    label: "S"; tip: ""
                    active: strip.track ? strip.track.soloed === true : false
                    onClicked: if (root.service && strip.track) root.service.toggleSolo(strip.track.id)
                    ControlHelp {
                        title: qsTr("Solo")
                        purpose: qsTr("Hear this track on its own. While any track is soloed, tracks that are not soloed stay silent.")
                        scope: qsTr("Solo several tracks to hear them together. The status bar shows SOLO while it is on.")
                    }
                }
                StateButton {
                    objectName: "stripRecordArm"
                    label: "R"; tip: ""
                    activeColor: Theme.record
                    active: root.service !== null && strip.track !== null && root.service.recordArmedTrack === strip.track.id
                    enabled: root.service !== null && !root.service.recording
                    onClicked: if (strip.track) root.service.setRecordArmedTrack(active ? -1 : strip.track.id)
                    ControlHelp {
                        title: qsTr("Arm for recording")
                        purpose: qsTr("Make this track the destination for the next recording.")
                        scope: qsTr("One track is armed at a time; arming this one disarms the other. Click again to disarm.")
                        unavailableReason: qsTr("Stop the recording in progress before changing the armed track.")
                    }
                }
            }

            FaderAndMeter {
                id: stripFaderItem
                objectName: "stripFader"
                helpTitle: qsTr("Level")
                helpText: qsTr("Set this track's level in decibels (dB). 0 dB leaves the source level unchanged.")
                helpScope: qsTr("Range −60 to +12 dB; double-click returns to 0 dB. Applies in playback and exports; the source audio is unchanged. Volume automation, when enabled, multiplies this level.")
                meterTitle: qsTr("Track peak meter")
                width: parent.width
                height: Math.max(implicitHeight, parent.height - y)
                db: strip.track ? strip.track.gainDb : 0
                silenced: strip.track ? strip.track.silenced === true : false
                peakLeft: strip.meter ? strip.meter.left : 0
                peakRight: strip.meter ? strip.meter.right : 0
                playing: root.service ? root.service.playing : false
                onMoved: function (value) {
                    if (root.service && strip.track)
                        root.service.setTrackGainDb(strip.track.id, value)
                }
                onReset: if (root.service && strip.track) root.service.setTrackGainDb(strip.track.id, 0)
                onTouched: pressed => { if (root.service && strip.track) root.service.automationGesture(strip.track.id, "volume", pressed) }
            }
        }
    }

    // The coloured cap of a strip: a role bar, the name, the role.
    component StripHeader: Rectangle {
        id: cap
        property color tint: Theme.audioPrimary
        property string name: ""
        property string role: ""
        property bool lit: false
        signal clicked()
        Accessible.role: Accessible.Button
        Accessible.name: cap.name
        Accessible.description: capHelp.description
        ControlHelp {
            id: capHelp
            title: cap.name
            purpose: qsTr("Click to make this the active track. Edits, the Inspector and the Track Effects panel act on the active track.")
            scope: qsTr("The role under the name sets the strip's colour; change it in the Inspector.")
        }

        height: Theme.hRow + Theme.s3
        radius: Theme.rControl
        color: Theme.withAlpha(tint, lit ? 0.34 : 0.2)
        border.width: lit ? Theme.hairline : 0
        border.color: tint

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 3
            topLeftRadius: Theme.rControl
            topRightRadius: Theme.rControl
            color: cap.tint
        }
        Column {
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.leftMargin: Theme.s1
            anchors.rightMargin: Theme.s1
            spacing: 0
            Text {
                objectName: "stripName"
                width: parent.width
                text: cap.name
                textFormat: Text.PlainText
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.weight: Theme.wHeading
                color: Theme.textPrimary
            }
            Text {
                width: parent.width
                text: cap.role
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsRuler
                color: Theme.textSecondary
            }
        }
        TapHandler { onTapped: cap.clicked() }
        ControlHelp {
            title: cap.name
            purpose: qsTr("The track's name and role. Click to focus the track.")
        }
    }

    // A vertical fader beside a stereo peak meter, the dB readout under them.
    component FaderAndMeter: Item {
        id: fm
        property real db: 0
        property bool silenced: false
        property real peakLeft: 0
        property real peakRight: 0
        property bool playing: false
        property string helpTitle: ""
        property string helpText: ""
        property string helpScope: ""
        property string meterTitle: ""
        signal moved(real db)
        signal reset()
        signal touched(bool pressed)

        // At least the standard fader; the strips stretch it to the panel's
        // height, so a tall mixer gets a long, finer fader instead of empty space.
        implicitHeight: Theme.hFader + Theme.hRow + Theme.s1
        readonly property real fraction: root.dbToFraction(fm.db)

        Item {
            id: body
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.wFader + Theme.s2 + Theme.wMiniMeter * 2 + 2 + 18
            height: fm.height - Theme.hRow - Theme.s1

            // Scale ticks, so a fader position means something without the readout.
            Repeater {
                model: [12, 0, -12, -24, -36, -48, -60]
                Item {
                    required property int modelData
                    x: 0
                    y: Math.round((1 - root.dbToFraction(modelData)) * (body.height - 1))
                    Rectangle { x: 14; y: 0; width: 4; height: 1; color: Theme.border }
                    Text {
                        x: 0; y: -Theme.fsRuler / 2 - 1
                        width: 13
                        horizontalAlignment: Text.AlignRight
                        text: String(parent.modelData)
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsRuler - 1
                        font.features: { "tnum": 1 }
                        color: Theme.textMuted
                    }
                }
            }

            // The fader.
            Rectangle {
                id: groove
                x: 20
                y: 0
                width: Theme.wFader
                height: parent.height
                radius: Theme.rControl
                color: Theme.meterBg
                border.width: Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.Slider
                Accessible.name: fm.helpTitle
                Accessible.description: faderHelp.description
                ControlHelp {
                    id: faderHelp
                    title: fm.helpTitle
                    purpose: fm.helpText
                    scope: fm.helpScope
                    suppressed: faderArea.pressed
                }

                Rectangle {
                    // The unity line, so 0 dB is findable by eye.
                    x: 1; width: parent.width - 2
                    y: Math.round((1 - root.dbToFraction(0)) * (parent.height - 1))
                    height: 1
                    color: Theme.borderStrong
                }
                Rectangle {
                    id: knob
                    x: -3
                    width: parent.width + 6
                    height: 14
                    radius: Theme.rControl
                    y: Math.round((1 - fm.fraction) * (parent.height - height))
                    color: faderArea.pressed ? Theme.accentPressed
                         : fm.silenced ? Theme.textMuted : Theme.textPrimary
                    border.width: Theme.hairline
                    border.color: Theme.borderStrong
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width - 6; height: 1
                        color: Theme.accent
                    }
                }
                MouseArea {
                    id: faderArea
                    objectName: "faderArea"
                    anchors.fill: parent
                    anchors.leftMargin: -4
                    anchors.rightMargin: -4
                    cursorShape: Qt.SizeVerCursor
                    preventStealing: true
                    function apply(y) {
                        const f = 1 - Math.max(0, Math.min(1, (y - knob.height / 2) / Math.max(1, groove.height - knob.height)))
                        fm.moved(root.fractionToDb(f))
                    }
                    property real grabOffset: 0
                    onPressed: function (e) {
                        const onKnob = e.y >= knob.y && e.y <= knob.y + knob.height
                        grabOffset = onKnob
                            ? e.y - ((1 - fm.fraction) * (groove.height - knob.height) + knob.height / 2) : 0
                        fm.touched(true)
                        if (!onKnob) apply(e.y)
                    }
                    onReleased: fm.touched(false)
                    onCanceled: fm.touched(false)
                    onPositionChanged: function (e) { if (pressed) apply(e.y - grabOffset) }
                    onDoubleClicked: fm.reset()
                }
            }

            // Two peak bars, with a held line and a sticky clip.
            Row {
                x: groove.x + groove.width + Theme.s1
                y: 0
                height: parent.height
                spacing: 2
                Accessible.role: Accessible.Indicator
                Accessible.name: fm.meterTitle
                Accessible.description: meterHelp.description
                ControlHelp {
                    id: meterHelp
                    title: fm.meterTitle
                    purpose: qsTr("Shows the peak level of the left and right channels after the effects and fader, from −60 to 0 dBFS. dBFS is level relative to the loudest a digital file can hold.")
                    scope: qsTr("The thin line is the held peak. A red border means the signal reached 0 dBFS; click the meter to clear it. The meter does not change the audio.")
                }
                MiniMeter { objectName: "stripMeter"; peak: fm.peakLeft; playing: fm.playing; height: parent.height }
                MiniMeter { objectName: "stripMeter"; peak: fm.peakRight; playing: fm.playing; height: parent.height }
            }
        }

        Rectangle {
            anchors.top: body.bottom
            anchors.topMargin: Theme.s1
            anchors.horizontalCenter: parent.horizontalCenter
            width: readout.implicitWidth + Theme.s2
            height: Theme.hRow - 2
            radius: Theme.rControl
            color: Theme.meterBg
            border.width: Theme.hairline
            border.color: Theme.border
            Text {
                id: readout
                objectName: "faderReadout"
                anchors.centerIn: parent
                text: (fm.db > 0 ? "+" : "") + fm.db.toFixed(1)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                font.features: { "tnum": 1 }
                color: fm.silenced ? Theme.textMuted : Theme.textSecondary
            }
        }
    }

    component MiniMeter: Rectangle {
        id: mm
        property real peak: 0
        property bool playing: false
        readonly property real db: peak > 0 ? 20 * Math.log(peak) / Math.LN10 : -200
        readonly property real fraction: Math.max(0, Math.min(1, (db + 60) / 60))
        property real held: 0
        property bool clipped: false

        onPeakChanged: {
            if (peak >= 1.0) clipped = true
            if (fraction > held) held = fraction
        }
        Timer {
            interval: 40; repeat: true
            running: mm.held > 0 && mm.playing
            onTriggered: mm.held = Math.max(mm.fraction, mm.held - 0.012)
        }

        width: Theme.wMiniMeter
        color: Theme.meterBg
        border.width: Theme.hairline
        border.color: clipped ? Theme.meterHigh : Theme.border

        Rectangle {
            x: 1
            width: parent.width - 2
            height: Math.round((parent.height - 2) * mm.fraction)
            y: parent.height - 1 - height
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: Theme.meterHigh }
                GradientStop { position: 0.04; color: Theme.meterHigh }
                GradientStop { position: 0.05; color: Theme.meterMid }
                GradientStop { position: 0.19; color: Theme.meterMid }
                GradientStop { position: 0.20; color: Theme.meterLow }
                GradientStop { position: 1.0; color: Theme.meterLow }
            }
        }
        Rectangle {
            visible: mm.held > 0.01
            x: 1; width: parent.width - 2; height: 1
            y: parent.height - 1 - Math.round((parent.height - 2) * mm.held)
            color: Theme.meterHold
        }
        MouseArea {
            anchors.fill: parent
            onClicked: { mm.clipped = false; mm.held = 0 }
            ControlHelp {
                title: qsTr("Peak meter")
                purpose: qsTr("The level of this track's output. A red frame means it clipped; click to clear the held peak and the clip light.")
            }
        }
    }
}
