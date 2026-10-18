import QtQuick
import "ui"

// One track's waveform, drawn on the window's shared time axis.
//
// A ShaderEffect sampling a peak texture. Phase 0 measured this against Canvas 2D and
// Qt Quick Shapes and it was the only approach that held 60fps with no measurable
// UI-thread cost (docs/phase0-results.md).
//
// Phase 4 moved the view state out into TimeAxis. This component now owns only what is
// specific to one lane: which pyramid to sample, whether it holds the selection, and
// the pointer gestures that land on it.
Item {
    id: root

    property var service: null
    property var axis: null
    property string shaderUrl: ""

    // The track this lane draws. -1 means the lane is not bound to one yet.
    property int trackId: -1
    // Which channel: -1 is the combined envelope (a Multitrack lane), 0.. is one
    // channel on its own (a Waveform page lane). Blocks in the texture; see
    // io/peak_texture.h.
    property int channel: -1
    property var levels: []
    property real trackFrames: 0
    property bool focused: false
    property bool clipSelectionEnabled: false
    property bool silenced: false

    property color waveColor: Theme.audioPrimary
    property color selectionColor: Theme.accent
    // The pointer over the waveform, in its own x.
    readonly property bool pointerInside: pointerHover.hovered
    readonly property real pointerX: pointerHover.point.position.x

    readonly property real framesPerPixel: axis ? axis.framesPerPixel : 1

    // Pick the pyramid level whose peaks-per-pixel stays inside the shader's 8-tap
    // bound. Without this the coarse end of the zoom range undersamples, which is
    // exactly what Phase 1 did with its single level.
    readonly property int level: {
        if (!levels || levels.length === 0)
            return 0
        for (var i = 0; i < levels.length; ++i) {
            if (root.framesPerPixel / levels[i].samplesPerPeak < 8.0)
                return i
        }
        return levels.length - 1
    }

    readonly property var activeLevel:
        (levels && level < levels.length) ? levels[level] : null

    property bool shaderFailed: false
    readonly property bool softwareRendering: GraphicsInfo.api === GraphicsInfo.Software || shaderFailed

    readonly property bool ready:
        service !== null && axis !== null && activeLevel !== null
        && (softwareRendering ? cpuWave.status === Image.Ready
                              : activeTexture !== null && activeTexture.status === Image.Ready)

    // A common time range is visible on every selected lane.
    readonly property bool holdsSelection:
        service !== null && service.selectedClips.length === 0 && !service.selectionEmpty && (service.selectionTrack === root.trackId
            || (service.selectedTracks.length > 1 && service.selectedTracks.indexOf(root.trackId) >= 0))
        && (root.channel < 0 || service.selectionChannel < 0 || service.selectionChannel === root.channel)

    signal seekRequested(real frame)   // a frame count: past 2^31 on a long take, so not an int
    signal selectionDragged(real startFrame, real endFrame, bool finished)
    signal focusRequested()
    signal clipPickRequested(real frame, bool toggle)

    function frameToX(frame) { return axis ? axis.frameToX(frame) : 0 }
    function xToFrame(x) { return axis ? axis.xToFrame(x) : 0 }

    // --- textures -----------------------------------------------------------------
    // Every level is preloaded. They are small (a 30-minute take is about 3 MB across
    // all seven levels) and preloading means crossing a zoom boundary costs nothing,
    // rather than stalling to decode a texture mid-gesture.

    property Item activeTexture: null

    function _refreshTexture() {
        root.activeTexture = textureRepeater.itemAt(root.level)
    }

    onLevelChanged: root._refreshTexture()
    onLevelsChanged: root._refreshTexture()

    Repeater {
        id: textureRepeater
        model: !root.softwareRendering && root.levels ? root.levels.length : 0

        delegate: Image {
            objectName: "waveformTexture"
            required property int index
            readonly property var levelData: root.levels[index]

            source: levelData ? levelData.textureUrl : ""
            sourceSize.width: levelData ? levelData.textureWidth : 0
            sourceSize.height: levelData ? levelData.textureHeight : 0
            smooth: false
            mipmap: false
            visible: false
            cache: false
            asynchronous: true

            onStatusChanged: if (status === Image.Ready) root._refreshTexture()
        }

        onItemAdded: root._refreshTexture()
        onItemRemoved: root._refreshTexture()
    }

    Loader {
        anchors.fill: parent
        active: root.ready && !root.softwareRendering
        sourceComponent: ShaderEffect {
        anchors.fill: parent
        // A silenced track still draws -- you need to see what you muted -- but it
        // reads as switched off rather than as part of the mix.
        opacity: root.silenced ? 0.3 : 1.0

        // Names must match the uniform block in wave.frag -- Qt binds by name using
        // the reflection data qsb embeds, not by declaration order.
        property variant peakTex: root.activeTexture
        property real offset: (root.activeLevel && root.axis)
                              ? root.axis.viewStart / root.activeLevel.samplesPerPeak : 0
        property real peaksPerPixel: root.activeLevel
                                     ? root.framesPerPixel / root.activeLevel.samplesPerPeak : 1
        property real itemWidth: width
        property real texWidth: root.activeLevel ? root.activeLevel.textureWidth : 1
        property real texHeight: root.activeLevel ? root.activeLevel.textureHeight : 1
        property real peakCount: root.activeLevel ? root.activeLevel.peakCount : 0
        property real block: {
            if (root.channel < 0 || !root.activeLevel)
                return 0
            const channels = root.activeLevel.channels !== undefined ? root.activeLevel.channels : 0
            return root.channel < channels ? root.channel + 1 : 0
        }
        property color waveColor: root.waveColor

        fragmentShader: root.shaderUrl
        onStatusChanged: if (status === ShaderEffect.Error) root.shaderFailed = true
        }
    }

    Image {
        id: cpuWave
        objectName: "cpuWaveform"
        anchors.fill: parent
        visible: root.softwareRendering
        opacity: root.silenced ? 0.3 : 1.0
        asynchronous: true
        cache: false
        smooth: false
        retainWhileLoading: true
        sourceSize: Qt.size(Math.ceil(width * Screen.devicePixelRatio), Math.ceil(height * Screen.devicePixelRatio))
        source: root.softwareRendering && root.activeLevel && root.axis && width > 0 && height > 0
            ? "image://audio-cpu/" + encodeURIComponent(JSON.stringify({
                kind: "wave", width: width, height: height, level: root.activeLevel,
                viewStart: root.axis.viewStart, framesPerPixel: root.framesPerPixel,
                channel: root.channel, colour: String(root.waveColor)
            })) : ""
    }

    // Centre line, so silence still reads as audio rather than as nothing loaded.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 1
        color: root.waveColor
        opacity: 0.25
    }

    // --- selection ----------------------------------------------------------------
    // While dragging, the raw drag is drawn locally so the gesture feels instant. The
    // engine's snapped answer replaces it on release -- immediate feedback, but the
    // authoritative range still comes from Session.

    property bool dragging: false
    property real dragAnchorFrame: 0
    property real dragCurrentFrame: 0

    Rectangle {
        id: selectionRect
        visible: root.dragging || root.holdsSelection
        color: root.selectionColor
        opacity: 0.18
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        readonly property real fromFrame: root.dragging
            ? Math.min(root.dragAnchorFrame, root.dragCurrentFrame)
            : (root.service ? root.service.selectionStart : 0)
        readonly property real toFrame: root.dragging
            ? Math.max(root.dragAnchorFrame, root.dragCurrentFrame)
            : (root.service ? root.service.selectionEnd : 0)

        x: root.frameToX(fromFrame)
        width: Math.max(1, root.frameToX(toFrame) - root.frameToX(fromFrame))
    }

    Repeater {
        model: 2
        delegate: Rectangle {
            required property int index
            visible: selectionRect.visible
            width: 1
            color: root.selectionColor
            opacity: 0.7
            // Sized off `root` rather than anchored to `parent`: a Repeater delegate
            // has no parent for the moment between being created and being reparented,
            // and anchoring through it logs a TypeError on every rebuild.
            y: 0
            height: root.height
            x: root.frameToX(index === 0 ? selectionRect.fromFrame : selectionRect.toFrame)
        }
    }

    // Where this track's audio ends, when it is shorter than the timeline. Without it
    // a short track and a long one look identical past the end of the short one.
    Rectangle {
        visible: root.trackFrames > 0 && root.axis !== null
                 && root.trackFrames < root.axis.totalFrames
        x: root.frameToX(root.trackFrames)
        width: Math.max(0, root.width - x)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        color: Theme.windowBg
        opacity: 0.4
    }

    // --- input --------------------------------------------------------------------

    MouseArea {
        id: mouse
        objectName: "waveMouse"
        anchors.fill: parent
        enabled: root.service !== null && root.trackId >= 0
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: enabled ? Qt.IBeamCursor : Qt.ArrowCursor

        property real pressX: 0
        property bool moved: false
        property bool clipPicking: false
        // Where the pointer is over the waveform, for what is drawn over it
        // (a clip's fade handles), which must not take the hover for itself.
        HoverHandler { id: pointerHover }
        ControlHelp {
            title: qsTr("Waveform")
            purpose: qsTr("Click to place the playhead. Drag to select a time range. Right-click for the track menu. The wheel scrolls; with Ctrl it zooms.")
            scope: root.clipSelectionEnabled ? qsTr("Ctrl-click toggles whole clips. Range Cut, Copy and Delete use the selected tracks; processing uses the focus.")
                                             : qsTr("Edits act on the selected range of the active track.")
            suppressed: mouse.pressed
        }

        onPressed: function (event) {
            pressX = event.x
            moved = false
            clipPicking = root.clipSelectionEnabled && (event.modifiers & Qt.ControlModifier) !== 0
            if (clipPicking) {
                root.clipPickRequested(Math.round(root.xToFrame(event.x)), true)
                return
            }
            root.focusRequested()
            root.dragAnchorFrame = root.xToFrame(event.x)
            root.dragCurrentFrame = root.dragAnchorFrame
        }

        onPositionChanged: function (event) {
            if (!pressed || clipPicking)
                return
            // A few pixels of slop, so a slightly shaky click still seeks rather than
            // creating a one-frame selection nobody asked for.
            if (!moved && Math.abs(event.x - pressX) < Theme.dragThreshold)
                return
            moved = true
            root.dragging = true
            root.dragCurrentFrame = root.xToFrame(event.x)
        }

        onCanceled: { root.dragging = false; moved = false; clipPicking = false }

        onReleased: function (event) {
            if (clipPicking) { clipPicking = false; return }
            if (root.dragging) {
                root.dragging = false
                root.selectionDragged(Math.round(root.dragAnchorFrame),
                                      Math.round(root.xToFrame(event.x)), true)
            } else {
                // A plain click positions the playhead and drops any selection.
                root.seekRequested(Math.round(root.xToFrame(event.x)))
                root.selectionDragged(0, 0, true)
            }
        }
    }

    // Wheel handling routes through the keymap like everything else -- section 6
    // allows no hardcoded bindings, and a wheel gesture with a modifier is a binding.
    signal wheelAction(string action, real deltaY, real x)

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) {
            root.wheelAction(root._wheelActionName(event.modifiers),
                             root.axis ? root.axis.wheelDelta(event) : 0, event.x)
            event.accepted = true
        }
    }

    // Set by the window, which owns the keymap.
    property var wheelResolver: null

    function _wheelActionName(modifiers) {
        return root.wheelResolver ? root.wheelResolver(modifiers) : ""
    }
}
