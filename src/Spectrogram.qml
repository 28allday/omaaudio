pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// One channel's spectrogram on the window's shared time axis: the spectral
// cache's tiles, each a ShaderEffect over its own texture, placed where its
// columns fall; the selection as time × band; the tools that draw it.
//
// Two layers of tiles: the coarsest level covers the whole take and is always
// loaded (it is one small texture), so zooming never shows a hole; the level
// the zoom wants is drawn over it, only the tiles in view, decoded off the GUI
// thread. Same reasoning as Waveform.qml's pyramid, with the textures too big
// to preload.
Item {
    id: root

    property var service: null
    property var axis: null
    property string shaderUrl: ""
    // The engine's `spectral` map; see Backend::installSpectral for its shape.
    property var spectral: null
    property int channel: 0
    property int overlayChannel: -1
    property int trackId: -1
    property real trackFrames: 0

    // Display. All view-only: the cache is never rebuilt for these.
    property string frequencyScale: "log"          // "log" | "linear"
    property real rangeDb: 60
    property real gainDb: 0
    property real minHz: 20
    property var stops: Theme.spectralPalette("navy")

    // "time" | "rect" | "band" | "lasso" | "brush" | "spot" | "harmonic"
    property string tool: "rect"
    property bool snapping: true
    // The drawing tools: the brush's radius in pixels, and whether a new shape
    // adds to the mask or cuts from it (Alt while drawing flips it).
    property real brushRadius: 24
    property bool maskSubtract: false
    property bool maskIntersect: false
    // The outline colour of what the next shape will be.
    readonly property color opOutline: maskSubtract ? Theme.maskSubtractOutline
                                     : maskIntersect ? Theme.maskKeepOutline : Theme.maskOutline
    readonly property bool drawingTool: tool === "lasso" || tool === "brush" || tool === "spot"
    // The harmonic tool (A24): a click or a drag along a partial picks its
    // series. The ghost comb under the pointer shows what the settings will
    // cover before anything is added.
    readonly property bool harmonicTool: tool === "harmonic"
    property int harmonicPartials: 16
    property bool harmonicFundamental: true
    signal harmonicsPicked(real fromSeconds, real toSeconds, real hz, bool subtract, bool intersect, real seedSeconds)
    readonly property real sampleRate: (spectral && spectral.sampleRate) ? spectral.sampleRate
                                     : (service && service.sampleRate ? service.sampleRate : 48000)

    readonly property real nyquist: (spectral && spectral.sampleRate) ? spectral.sampleRate / 2 : 24000
    readonly property int hop: (spectral && spectral.hop) ? spectral.hop : 512
    readonly property int analysisHop: (spectral && spectral.analysisHop) ? spectral.analysisHop : hop
    readonly property var lane: (spectral && spectral.ready && spectral.perChannel
                                 && channel < spectral.perChannel.length)
                                ? spectral.perChannel[channel] : null
    readonly property var levels: lane ? lane.levels : []
    readonly property real framesPerPixel: axis ? axis.framesPerPixel : 1
    property bool shaderFailed: false
    readonly property bool softwareRendering: GraphicsInfo.api === GraphicsInfo.Software || shaderFailed
    readonly property bool ready: lane !== null && levels.length > 0 && axis !== null

    // The level whose columns-per-pixel stays under the shader's 8 taps.
    readonly property int level: {
        if (!levels || levels.length === 0)
            return 0
        for (var i = 0; i < levels.length; ++i) {
            if (root.framesPerPixel / (root.hop * levels[i].decimation) < 8.0)
                return i
        }
        return levels.length - 1
    }
    readonly property int coarsest: levels ? Math.max(0, levels.length - 1) : 0

    signal seekRequested(real frame)   // a frame count: past 2^31 on a long take, so not an int
    signal focusRequested()
    signal wheelAction(string action, real deltaY, real x)
    property var wheelResolver: null

    function frameToX(frame) { return axis ? axis.frameToX(frame) : 0 }
    function xToFrame(x) { return axis ? axis.xToFrame(x) : 0 }

    // Frequency ↔ y, the same mapping the shader uses.
    function hzToY(hz) {
        var up
        if (root.frequencyScale === "log") {
            const lo = Math.max(1, root.minHz)
            up = Math.log(Math.max(lo, hz) / lo) / Math.log(root.nyquist / lo)
        } else {
            up = hz / root.nyquist
        }
        return (1 - Math.max(0, Math.min(1, up))) * root.height
    }
    function yToHz(y) {
        const up = Math.max(0, Math.min(1, 1 - y / Math.max(1, root.height)))
        if (root.frequencyScale === "log") {
            const lo = Math.max(1, root.minHz)
            return lo * Math.pow(root.nyquist / lo, up)
        }
        return up * root.nyquist
    }

    function tileX(tile, decimation) {
        // A pooled column covers the original columns from its first hop onward;
        // its left edge stays half an analysis hop before that first centre.
        return root.frameToX(tile.firstColumn * root.hop * decimation - root.analysisHop / 2)
    }
    function tileWidth(tile, decimation) {
        return tile.columns * root.hop * decimation / root.framesPerPixel
    }

    // The tiles of the active level that touch the view, plus one either side.
    readonly property var visibleTiles: {
        if (!root.ready || !root.axis)
            return []
        const lvl = root.levels[root.level]
        const framesPerColumn = root.hop * lvl.decimation
        const first = Math.max(0, Math.floor(root.axis.viewStart / framesPerColumn / (root.spectral.tileColumns)) - 1)
        const last = Math.floor(root.axis.viewEnd / framesPerColumn / (root.spectral.tileColumns)) + 1
        var out = []
        for (var i = first; i <= last && i < lvl.tiles.length; ++i)
            out.push(lvl.tiles[i])
        return out
    }

    // One tile: its texture and the shader that draws it. Sized off `root`, not
    // `parent` (trap 38).
    component Tile: Item {
        id: tile
        required property var modelData
        required property int index
        property int decimation: 1
        property real opacityWhenReady: 1.0
        property var tilePalette: root.overlayChannel >= 0 ? ["#071019", "#082b3d", "#087994", "#39c7e0", "#c2fcff"] : root.stops
        x: root.tileX(modelData, decimation)
        y: 0
        width: root.tileWidth(modelData, decimation)
        height: root.height

        Image {
            id: tex
            objectName: "spectralTexture"
            source: tile.modelData ? tile.modelData.textureUrl : ""
            sourceSize.width: tile.modelData ? tile.modelData.width : 0
            sourceSize.height: tile.modelData ? tile.modelData.height : 0
            smooth: false
            mipmap: false
            visible: false
            cache: false
            asynchronous: true
        }

        ShaderEffect {
            objectName: "spectralTile"
            anchors.fill: parent
            visible: tex.status === Image.Ready
            opacity: tile.opacityWhenReady
            property variant tileTex: tex
            property real columns: tile.modelData ? tile.modelData.columns : 1
            property real bins: tile.modelData ? tile.modelData.height : 1
            property real itemWidth: width
            property real itemHeight: height
            property real logScale: root.frequencyScale === "log" ? 1 : 0
            property real minHz: root.minHz
            property real nyquist: root.nyquist
            property real floorDb: root.spectral && root.spectral.floorDb !== undefined ? root.spectral.floorDb : -120
            property real rangeDb: root.rangeDb
            property real gainDb: root.gainDb
            property color c0: tile.tilePalette[0]
            property color c1: tile.tilePalette[1]
            property color c2: tile.tilePalette[2]
            property color c3: tile.tilePalette[3]
            property color c4: tile.tilePalette[4]
            fragmentShader: root.shaderUrl
            onStatusChanged: if (status === ShaderEffect.Error) root.shaderFailed = true
        }
    }

    Item {
        anchors.fill: parent
        clip: true
        visible: !root.softwareRendering

        // The base: the coarsest level, whole take.
        Repeater {
            model: root.ready && !root.softwareRendering ? root.levels[root.coarsest].tiles : []
            delegate: Tile {
                decimation: root.ready ? root.levels[root.coarsest].decimation : 1
            }
        }
        // The level the zoom asks for, where it is looking.
        Repeater {
            model: root.ready && !root.softwareRendering && root.level !== root.coarsest ? root.visibleTiles : []
            delegate: Tile {
                decimation: root.ready ? root.levels[root.level].decimation : 1
            }
        }

        // A second channel on the same axis, flattened before blending so
        // overlapping cache levels do not change its opacity.
        Item {
            anchors.fill: parent
            layer.enabled: root.overlayChannel >= 0
            opacity: 0.5
            Repeater {
                model: root.ready && !root.softwareRendering && root.overlayChannel >= 0
                    ? root.spectral.perChannel[root.overlayChannel].levels[root.coarsest].tiles : []
                delegate: Tile {
                    decimation: root.ready ? root.levels[root.coarsest].decimation : 1
                    tilePalette: ["#071019", "#392514", "#91611c", "#e7b23e", "#fff0bd"]
                }
            }
            Repeater {
                model: root.ready && !root.softwareRendering && root.overlayChannel >= 0 && root.level !== root.coarsest
                    ? root.spectral.perChannel[root.overlayChannel].levels[root.level].tiles.filter(function(t) {
                        return root.tileX(t, root.levels[root.level].decimation) < root.width
                            && root.tileX(t, root.levels[root.level].decimation) + root.tileWidth(t, root.levels[root.level].decimation) > 0
                    }) : []
                delegate: Tile {
                    decimation: root.ready ? root.levels[root.level].decimation : 1
                    tilePalette: ["#071019", "#392514", "#91611c", "#e7b23e", "#fff0bd"]
                }
            }
        }

        // Past the end of the take.
        Rectangle {
            visible: root.trackFrames > 0 && root.axis !== null && root.trackFrames < root.axis.totalFrames
            x: root.frameToX(root.trackFrames)
            width: Math.max(0, root.width - x)
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            color: Theme.windowBg
            opacity: 0.6
        }
    }

    // Assemble only cache tiles touching the viewport. Rendering and decoding
    // happen in the provider, never in a QML paint loop or the audio callback.
    function cpuLayer(channelIndex, palette) {
        const sourceLevels = root.spectral.perChannel[channelIndex].levels
        var tiles = []
        const append = function(levelIndex) {
            const sourceLevel = sourceLevels[levelIndex]
            const span = root.hop * sourceLevel.decimation * root.spectral.tileColumns
            const first = Math.max(0, Math.floor(root.axis.viewStart / span) - 1)
            const last = Math.min(sourceLevel.tiles.length - 1, Math.floor(root.axis.viewEnd / span) + 1)
            for (var i = first; i <= last; ++i) {
                const t = sourceLevel.tiles[i]
                const x = root.tileX(t, sourceLevel.decimation)
                const w = root.tileWidth(t, sourceLevel.decimation)
                if (x < root.width && x + w > 0)
                    tiles.push({ texture: t.texture, asset: t.asset, columns: t.columns, x: x, width: w })
            }
        }
        append(root.coarsest)
        if (root.level !== root.coarsest) append(root.level)
        return { palette: palette, tiles: tiles }
    }
    Image {
        id: cpuSpectral
        objectName: "cpuSpectrogram"
        anchors.fill: parent
        visible: root.softwareRendering
        asynchronous: true
        cache: false
        smooth: false
        retainWhileLoading: true
        sourceSize: Qt.size(Math.ceil(width * Screen.devicePixelRatio), Math.ceil(height * Screen.devicePixelRatio))
        source: {
            // Explicit axis dependencies also track movement with unchanged levels.
            const start = root.axis ? root.axis.viewStart : 0
            const fpp = root.framesPerPixel
            if (!root.softwareRendering || !root.ready || width <= 0 || height <= 0) return ""
            const overlay = root.overlayChannel >= 0
            var layers = [root.cpuLayer(root.channel, overlay ? ["#071019", "#082b3d", "#087994", "#39c7e0", "#c2fcff"] : root.stops)]
            if (overlay) layers.push(root.cpuLayer(root.overlayChannel, ["#071019", "#392514", "#91611c", "#e7b23e", "#fff0bd"]))
            return "image://audio-cpu/" + encodeURIComponent(JSON.stringify({
                kind: "spectral", width: width, height: height, viewStart: start, framesPerPixel: fpp,
                layers: layers, log: root.frequencyScale === "log", minHz: root.minHz,
                nyquist: root.nyquist, rangeDb: root.rangeDb, gainDb: root.gainDb,
                floorDb: root.spectral.floorDb !== undefined ? root.spectral.floorDb : -120
            }))
        }
    }
    Rectangle {
        visible: root.softwareRendering && root.trackFrames > 0 && root.axis !== null && root.trackFrames < root.axis.totalFrames
        x: root.frameToX(root.trackFrames)
        width: Math.max(0, root.width - x)
        height: root.height
        color: Theme.windowBg
        opacity: 0.6
    }

    // --- selection ----------------------------------------------------------------
    // Time from the engine's selection, band from its band. While dragging, the
    // raw gesture is drawn locally and the engine's answer replaces it on release.

    readonly property bool holdsSelection:
        service !== null && !service.selectionEmpty && service.selectionTrack === root.trackId
    readonly property bool bandEmpty: service ? service.bandEmpty : true

    property bool dragging: false
    property real dragAnchorFrame: 0
    property real dragCurrentFrame: 0
    property real dragAnchorY: 0
    property real dragCurrentY: 0

    readonly property real selFrom: dragging ? Math.min(dragAnchorFrame, dragCurrentFrame)
                                             : (service ? service.selectionStart : 0)
    readonly property real selTo: dragging ? Math.max(dragAnchorFrame, dragCurrentFrame)
                                           : (service ? service.selectionEnd : 0)
    readonly property bool dragBand: dragging && root.tool !== "time" && !root.harmonicTool
    readonly property real bandTop: dragBand ? Math.min(dragAnchorY, dragCurrentY)
                                  : (bandEmpty ? 0 : root.hzToY(service.bandHighHz))
    readonly property real bandBottom: dragBand ? Math.max(dragAnchorY, dragCurrentY)
                                     : (bandEmpty ? root.height : root.hzToY(service.bandLowHz))
    readonly property bool showSelection: (dragging && root.tool !== "band") || holdsSelection
    // Where the harmonic tool's comb would sit: at the pointer, or at the press
    // while a span is being dragged.
    readonly property real harmonicHz: mouse.pressed && root.harmonicTool ? root.yToHz(mouse.pressY)
                                     : root.yToHz(Math.max(0, Math.min(root.height, mouse.hoverY)))
    readonly property bool showBandOnly: !showSelection && !bandEmpty && !dragging

    Rectangle {
        id: selectionRect
        objectName: "spectralSelection"
        visible: root.showSelection || root.dragBand
        color: Theme.selectionFill
        border.width: Theme.hairline
        border.color: Theme.selectionOutline
        x: (root.tool === "band" && root.dragging) || (!root.dragging && !root.holdsSelection)
           ? 0 : root.frameToX(root.selFrom)
        width: (root.tool === "band" && root.dragging) || (!root.dragging && !root.holdsSelection)
               ? root.width : Math.max(1, root.frameToX(root.selTo) - root.frameToX(root.selFrom))
        y: root.bandTop
        height: Math.max(1, root.bandBottom - root.bandTop)
    }
    // A band with no time selection: two lines across the whole lane.
    Repeater {
        model: root.showBandOnly ? 2 : 0
        delegate: Rectangle {
            required property int index
            x: 0; width: root.width; height: 1
            y: index === 0 ? root.bandTop : root.bandBottom
            color: Theme.selectionOutline
            opacity: 0.7
        }
    }
    // The band's edges, in Hz, at the selection's corners (the concept's readouts).
    Repeater {
        model: (selectionRect.visible && !root.bandEmpty) || root.dragBand ? 2 : 0
        delegate: Rectangle {
            required property int index
            objectName: "bandReadout"
            readonly property real hz: index === 0 ? root.yToHz(root.bandTop) : root.yToHz(root.bandBottom)
            x: Math.max(0, Math.min(root.width - width, selectionRect.x + Theme.s1))
            y: index === 0 ? Math.max(0, selectionRect.y - height - 2)
                           : Math.min(root.height - height, selectionRect.y + selectionRect.height + 2)
            width: label.implicitWidth + Theme.s2
            height: label.implicitHeight + 2
            color: Theme.panelRaised
            border.width: Theme.hairline
            border.color: Theme.selectionOutline
            radius: Theme.rControl
            Text {
                id: label
                anchors.centerIn: parent
                text: Theme.formatHz(parent.hz)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsRuler
                font.features: { "tnum": 1 }
                color: Theme.textPrimary
            }
        }
    }

    // --- the mask -----------------------------------------------------------------
    // The engine's shapes, in seconds and Hz, drawn through the same mappings the
    // selection uses, so what is on screen is what the repair will rasterise. The
    // stroke being drawn is painted on top until it is handed over on release.

    readonly property var maskShapes: service ? service.maskShapes : []
    readonly property int maskCount: maskShapes ? maskShapes.length : 0

    function secondsToX(seconds) { return root.frameToX(seconds * root.sampleRate) }
    function xToSeconds(x) { return root.xToFrame(x) / root.sampleRate }
    function pointAt(x, y) {
        return { seconds: root.xToSeconds(x), hz: root.yToHz(Math.max(0, Math.min(root.height, y))) }
    }
    // One dab at (x, y): round on screen, whatever the scale.
    function stampAt(x, y) {
        const r = Math.max(1, root.brushRadius)
        const yy = Math.max(0, Math.min(root.height, y))
        return { seconds: root.xToSeconds(x), hz: root.yToHz(yy),
                 rSeconds: r * root.framesPerPixel / root.sampleRate,
                 hzLow: root.yToHz(Math.min(root.height, yy + r)),
                 hzHigh: root.yToHz(Math.max(0, yy - r)) }
    }

    // A stamp's vertical extent at a horizontal offset, the engine's own curve.
    function stampPath(ctx, st) {
        const cx = root.secondsToX(st.seconds)
        const rx = Math.max(1, st.rSeconds * root.sampleRate / root.framesPerPixel)
        const yc = root.hzToY(st.hz)
        const yTop = root.hzToY(st.hzHigh)
        const yBottom = root.hzToY(st.hzLow)
        const steps = 16
        ctx.beginPath()
        for (var i = 0; i <= steps; ++i) {
            const dt = -1 + 2 * i / steps
            const sc = Math.sqrt(Math.max(0, 1 - dt * dt))
            const x = cx + dt * rx
            const y = yc - (yc - yTop) * sc
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
        }
        for (var j = steps; j >= 0; --j) {
            const dt2 = -1 + 2 * j / steps
            const sc2 = Math.sqrt(Math.max(0, 1 - dt2 * dt2))
            ctx.lineTo(cx + dt2 * rx, yc + (yBottom - yc) * sc2)
        }
        ctx.closePath()
    }

    function paintShape(ctx, shape, live) {
        const op = shape.op !== undefined ? shape.op : (shape.subtract === true ? "cut" : "add")
        ctx.fillStyle = op === "cut" ? Theme.maskSubtractFill : op === "keep" ? Theme.maskKeepFill : Theme.maskFill
        ctx.strokeStyle = op === "cut" ? Theme.maskSubtractOutline : op === "keep" ? Theme.maskKeepOutline : Theme.maskOutline
        ctx.lineWidth = live ? 1.5 : 1
        if (shape.kind === "comb") {
            if (shape.pitch && shape.pitch.length >= 2) {
                const points = shape.pitch
                const ratio = Math.pow(2, shape.width / 1200) - 1
                let lowest = root.nyquist
                for (let p of points) lowest = Math.min(lowest, p.hz)
                for (let n = shape.includeFundamental ? 1 : 2; n <= shape.partials; ++n) {
                    const lowestBand = lowest * n - (shape.widthInHz ? shape.width : lowest * n * ratio)
                    if (lowestBand > root.nyquist) break
                    ctx.beginPath()
                    for (let side = 0; side < 2; ++side) {
                        // Extend the endpoint frequencies across any uncovered
                        // leading/trailing gap, as the repair mask does.
                        for (let j = 0; j < points.length + 2; ++j) {
                            const index = side === 0 ? j - 1 : points.length - j
                            const p = points[Math.max(0, Math.min(points.length - 1, index))]
                            const hz = p.hz * n
                            const half = shape.widthInHz ? shape.width : hz * ratio
                            const y = root.hzToY(Math.max(0, Math.min(root.nyquist, hz + (side === 0 ? half : -half))))
                            const x = root.secondsToX(index < 0 ? shape.from : index >= points.length ? shape.to : p.seconds)
                            if (side === 0 && j === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                    }
                    ctx.closePath()
                    ctx.fill()
                    ctx.stroke()
                }
                return
            }
            // One rectangle per partial, over the comb's own span.
            const bands = shape.bands
            if (!bands)
                return
            const x0 = root.secondsToX(shape.from)
            const x1 = root.secondsToX(shape.to)
            const w = Math.max(1, x1 - x0)
            for (var b = 0; b < bands.length; ++b) {
                const yTop = root.hzToY(bands[b].hzHigh)
                const yBottom = root.hzToY(bands[b].hzLow)
                const hh = Math.max(1, yBottom - yTop)
                ctx.fillRect(x0, yTop, w, hh)
                ctx.strokeRect(x0 + 0.5, yTop + 0.5, w - 1, hh - 1)
            }
            return
        }
        if (shape.kind === "lasso") {
            const pts = shape.points
            if (!pts || pts.length < 2)
                return
            ctx.beginPath()
            for (var i = 0; i < pts.length; ++i) {
                const x = root.secondsToX(pts[i].seconds)
                const y = root.hzToY(pts[i].hz)
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.closePath()
            if (pts.length >= 3)
                ctx.fill()
            ctx.stroke()
        } else {
            const dabs = shape.stamps
            if (!dabs)
                return
            for (var k = 0; k < dabs.length; ++k) {
                root.stampPath(ctx, dabs[k])
                ctx.fill()
            }
            // The outline once, over the union, so a stroke reads as one shape.
            ctx.globalAlpha = 0.9
            for (var m = 0; m < dabs.length; ++m) {
                root.stampPath(ctx, dabs[m])
                ctx.stroke()
            }
            ctx.globalAlpha = 1
        }
    }

    Canvas {
        id: maskCanvas
        objectName: "maskCanvas"
        anchors.fill: parent
        visible: root.maskCount > 0 || mouse.drawing
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            for (var i = 0; i < root.maskCount; ++i)
                root.paintShape(ctx, root.maskShapes[i], false)
            if (mouse.drawing && mouse.live !== null)
                root.paintShape(ctx, mouse.live, true)
        }
        function refresh() { if (visible) requestPaint() }
        Connections {
            target: root
            function onMaskShapesChanged() { maskCanvas.refresh() }
            function onFramesPerPixelChanged() { maskCanvas.refresh() }
            function onFrequencyScaleChanged() { maskCanvas.refresh() }
            function onWidthChanged() { maskCanvas.refresh() }
            function onHeightChanged() { maskCanvas.refresh() }
        }
        Connections {
            target: root.axis
            function onViewStartChanged() { maskCanvas.refresh() }
        }
        onVisibleChanged: refresh()
    }

    // The clicks the detector found: a hairline each, the lane's full height, with
    // a tick at the top so a one-frame event is still visible at any zoom.
    readonly property var clickList: service ? service.clicks : []
    Repeater {
        model: root.clickList
        delegate: Item {
            id: mark
            objectName: "clickMark"
            required property var modelData
            readonly property real xAt: root.frameToX(modelData.frame + modelData.length / 2)
            x: xAt
            y: 0
            width: 1
            height: root.height
            visible: xAt >= -1 && xAt <= root.width + 1
            Rectangle { anchors.fill: parent; color: Theme.warning; opacity: 0.55 }
            Rectangle {
                x: -3; y: 0; width: 7; height: 7
                color: Theme.warning
                rotation: 45
                opacity: 0.95
            }
        }
    }

    // The ghost comb under the harmonic tool: a hairline at every partial the
    // settings would take, across the span being dragged or the whole lane.
    Repeater {
        model: root.harmonicTool && mouse.enabled && (mouse.containsMouse || mouse.pressed)
               ? Math.max(0, root.harmonicPartials - (root.harmonicFundamental ? 0 : 1)) : 0
        delegate: Rectangle {
            id: ghost
            objectName: "harmonicGhost"
            required property int index
            readonly property int n: index + (root.harmonicFundamental ? 1 : 2)
            readonly property real yAt: root.hzToY(root.harmonicHz * n)
            readonly property bool spanning: mouse.pressed && root.dragging
            x: spanning ? Math.min(root.frameToX(root.dragAnchorFrame), root.frameToX(root.dragCurrentFrame)) : 0
            width: spanning ? Math.max(1, Math.abs(root.frameToX(root.dragCurrentFrame) - root.frameToX(root.dragAnchorFrame)))
                            : root.width
            y: yAt
            height: 1
            visible: root.harmonicHz * n < root.nyquist && yAt >= 0 && yAt <= root.height
            color: root.opOutline
            opacity: n === 1 ? 0.95 : 0.6
        }
    }

    // The brush, where the pointer is.
    Rectangle {
        objectName: "brushCursor"
        visible: (root.tool === "brush" || root.tool === "spot") && mouse.containsMouse && mouse.enabled
        x: mouse.hoverX - width / 2
        y: mouse.hoverY - height / 2
        width: Math.max(2, root.brushRadius * 2)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 1
        border.color: root.tool === "spot" ? Theme.warning : root.opOutline
        opacity: 0.9
    }

    // --- input --------------------------------------------------------------------

    MouseArea {
        id: mouse
        objectName: "spectralMouse"
        anchors.fill: parent
        enabled: root.service !== null && root.trackId >= 0
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        hoverEnabled: true
        cursorShape: !enabled ? Qt.ArrowCursor
                   : (root.tool === "brush" || root.tool === "spot") ? Qt.BlankCursor : Qt.CrossCursor

        property real pressX: 0
        property real pressY: 0
        property bool moved: false
        property real hoverX: 0
        property real hoverY: 0
        ControlHelp {
            title: qsTr("Spectrogram")
            purpose: root.tool === "lasso" ? qsTr("Time runs across, frequency up; brighter is louder. Drag to draw a lasso round the sound to repair.")
                   : root.tool === "brush" ? qsTr("Time runs across, frequency up; brighter is louder. Paint over the sound to repair; the wheel changes the brush size.")
                   : root.tool === "spot" ? qsTr("Time runs across, frequency up; brighter is louder. Click a spot to mark it for repair.")
                   : qsTr("Time runs across, frequency up; brighter is louder. Drag to select a band of time and frequency; click to place the playhead.")
            scope: qsTr("Alt while drawing flips Add and Cut. The wheel scrolls; with Ctrl it zooms.")
            suppressed: mouse.pressed
        }

        // The shape being drawn, in the engine's own terms.
        property bool drawing: false
        property var live: null
        property bool subtractNow: false
        property bool intersectNow: false
        property real lastStampX: 0
        property real lastStampY: 0

        function beginShape(event) {
            // Alt flips add and cut; a keep with Alt is a cut.
            const alt = (event.modifiers & Qt.AltModifier) !== 0
            subtractNow = root.maskSubtract !== alt
            intersectNow = root.maskIntersect && !alt
            const op = root.tool === "spot" ? "add" : intersectNow ? "keep" : subtractNow ? "cut" : "add"
            if (root.tool === "lasso") {
                live = { kind: "lasso", op: op, subtract: op === "cut", points: [root.pointAt(event.x, event.y)] }
            } else {
                live = { kind: "stroke", op: op, subtract: op === "cut",
                         stamps: [root.stampAt(event.x, event.y)] }
                lastStampX = event.x
                lastStampY = event.y
            }
            drawing = true
            maskCanvas.refresh()
        }
        function extendShape(x, y) {
            if (live === null)
                return
            if (live.kind === "lasso") {
                live.points.push(root.pointAt(x, y))
            } else {
                // Dabs every third of a radius along the path, so a fast stroke
                // has no gaps.
                const step = Math.max(2, root.brushRadius / 3)
                const dx = x - lastStampX
                const dy = y - lastStampY
                const dist = Math.sqrt(dx * dx + dy * dy)
                if (dist < step)
                    return
                const n = Math.floor(dist / step)
                for (var i = 1; i <= n; ++i) {
                    const px = lastStampX + dx * i / n
                    const py = lastStampY + dy * i / n
                    live.stamps.push(root.stampAt(px, py))
                }
                lastStampX = x
                lastStampY = y
            }
            maskCanvas.refresh()
        }
        function finishShape() {
            const shape = live
            drawing = false
            live = null
            maskCanvas.refresh()
            if (!root.service || shape === null)
                return
            if (shape.kind === "lasso") {
                if (shape.points.length >= 3)
                    root.service.addMaskLasso(shape.points, shape.op === "cut", shape.op === "keep")
            } else if (root.tool === "spot") {
                root.service.spotHeal(shape.stamps)
            } else {
                root.service.addMaskStroke(shape.stamps, shape.op === "cut", shape.op === "keep")
            }
        }

        onPositionChanged: function (event) {
            hoverX = event.x
            hoverY = event.y
            if (!pressed)
                return
            if (root.drawingTool) {
                if (drawing)
                    extendShape(event.x, event.y)
                return
            }
            if (!moved && Math.abs(event.x - pressX) < 3 && Math.abs(event.y - pressY) < 3)
                return
            if (root.harmonicTool && !moved && Math.abs(event.x - pressX) < 3)
                return   // a span is dragged along the partial, not up or down
            moved = true
            root.dragging = true
            root.dragCurrentFrame = root.xToFrame(event.x)
            root.dragCurrentY = Math.max(0, Math.min(root.height, event.y))
        }
        onPressed: function (event) {
            pressX = event.x
            pressY = event.y
            moved = false
            root.focusRequested()
            if (root.drawingTool) {
                beginShape(event)
                return
            }
            root.dragAnchorFrame = root.xToFrame(event.x)
            root.dragCurrentFrame = root.dragAnchorFrame
            root.dragAnchorY = event.y
            root.dragCurrentY = event.y
        }
        onReleased: function (event) {
            // By what the press began, not by the tool selected now: a hotkey
            // can change the tool while the button is down. A stroke under way
            // is finished as a stroke; a drag that became a drawing tool is dropped.
            if (drawing) {
                finishShape()
                return
            }
            if (root.drawingTool) {
                root.dragging = false
                return
            }
            if (root.harmonicTool) {
                // A click: the series over the selection there is (or the take).
                // A drag: over the span dragged. Alt cuts, like the drawing tools.
                const alt = (event.modifiers & Qt.AltModifier) !== 0
                const cut = root.maskSubtract !== alt
                const keep = root.maskIntersect && !alt
                const hz = root.yToHz(Math.max(0, Math.min(root.height, pressY)))
                if (!root.dragging) {
                    root.harmonicsPicked(0, 0, hz, cut, keep, root.xToSeconds(pressX))
                    return
                }
                root.dragging = false
                const s0 = root.xToSeconds(Math.min(pressX, event.x))
                const s1 = root.xToSeconds(Math.max(pressX, event.x))
                if (s1 > s0)
                    root.harmonicsPicked(s0, s1, hz, cut, keep, root.xToSeconds(pressX))
                return
            }
            if (!root.dragging) {
                // A click seeks and drops the selection, as on the Waveform page --
                // and the mask with it.
                root.seekRequested(Math.round(root.xToFrame(event.x)))
                if (root.service) {
                    root.service.clearSelection()
                    root.service.clearBand()
                    root.service.clearMask()
                }
                return
            }
            root.dragging = false
            if (!root.service)
                return
            const a = Math.round(root.dragAnchorFrame)
            const b = Math.round(root.xToFrame(event.x))
            const yTop = Math.min(root.dragAnchorY, root.dragCurrentY)
            const yBottom = Math.max(root.dragAnchorY, root.dragCurrentY)
            if (root.tool === "time") {
                root.service.clearBand()
                if (a !== b)
                    root.service.select(a, b, root.snapping, root.trackId)
            } else if (root.tool === "rect") {
                if (a !== b)
                    root.service.select(a, b, root.snapping, root.trackId)
                root.service.setBand(root.yToHz(yBottom), root.yToHz(yTop))
            } else {
                // Band: the frequencies, over the time selection there is -- or
                // the whole take.
                if (root.service.selectionEmpty || root.service.selectionTrack !== root.trackId)
                    root.service.select(0, root.trackFrames, false, root.trackId)
                root.service.setBand(root.yToHz(yBottom), root.yToHz(yTop))
            }
        }
        onCanceled: {
            root.dragging = false
            moved = false
            if (drawing) {
                drawing = false
                live = null
                maskCanvas.refresh()
            }
        }
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) {
            root.wheelAction(root.wheelResolver ? root.wheelResolver(event.modifiers) : "",
                             root.axis ? root.axis.wheelDelta(event) : 0, event.x)
            event.accepted = true
        }
    }
}
