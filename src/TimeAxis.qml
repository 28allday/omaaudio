import QtQuick

// The view onto the timeline, shared by every track lane.
//
// One axis object for the whole window is what makes a stack of lanes read as one
// timeline rather than as several unrelated views: zoom and scroll move all of them
// together because there is only one place the numbers live.
//
// The view is defined by two numbers -- which frame sits at the left edge, and how
// many frames each pixel covers. Everything else derives from those, which keeps zoom
// continuous rather than stepped through the pyramid levels.
QtObject {
    id: root

    // Set by the window: the pixel width of a lane's waveform area, and the length of
    // the longest track.
    property real viewWidth: 0
    property real totalFrames: 0

    property real viewStart: 0            // frame at the left edge
    property real framesPerPixel: 1

    // Never zoom past one frame per pixel, and never out past the whole take in view.
    readonly property real minFramesPerPixel: 1.0
    readonly property real maxFramesPerPixel: root._maxFpp()

    readonly property real viewFrames: framesPerPixel * viewWidth
    readonly property real viewEnd: viewStart + viewFrames

    // Deliberately a function, and deliberately used by every function below instead
    // of the property above.
    //
    // Reading maxFramesPerPixel from inside a change handler for something it depends
    // on returned a stale 1 while totalFrames and viewWidth were already correct: the
    // derived binding had not been re-evaluated yet at the moment the handler ran.
    // That pinned the view at one frame per pixel and looked exactly like zoom was
    // broken. Computing from the primitives cannot go stale.
    function _maxFpp() {
        return (root.totalFrames > 0 && root.viewWidth > 0)
            ? Math.max(1, root.totalFrames / root.viewWidth)
            : 1
    }

    function frameToX(frame) {
        if (root.framesPerPixel <= 0)
            return 0
        return (frame - root.viewStart) / root.framesPerPixel
    }

    function xToFrame(x) {
        return root.viewStart + x * root.framesPerPixel
    }

    function clampView() {
        // A window being laid out reports a width of zero, or briefly a nonsense one.
        // Clamping against that collapses the view to one frame per pixel and looks
        // exactly like the zoom is broken, so transient widths are ignored outright.
        if (root.viewWidth <= 0)
            return
        root.framesPerPixel = Math.max(root.minFramesPerPixel,
                                       Math.min(root._maxFpp(), root.framesPerPixel))
        const maxStart = Math.max(0, root.totalFrames
                                     - root.framesPerPixel * root.viewWidth)
        root.viewStart = Math.max(0, Math.min(maxStart, root.viewStart))
    }

    // Zoom about a pixel position, so whatever is under the pointer stays put.
    function zoomAt(x, factor) {
        if (root.totalFrames <= 0 || root.viewWidth <= 0)
            return
        root.autoFit = false
        const anchorFrame = root.xToFrame(x)
        root.framesPerPixel = Math.max(root.minFramesPerPixel,
                                       Math.min(root._maxFpp(),
                                                root.framesPerPixel * factor))
        root.viewStart = anchorFrame - x * root.framesPerPixel
        root.clampView()
    }

    function wheelDelta(event) {
        const pixels = event.pixelDelta
        if (pixels.x !== 0 || pixels.y !== 0)
            return (pixels.y !== 0 ? pixels.y : pixels.x) * 1.5
        return event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
    }

    function applyWheel(action, delta, x) {
        if (!Number.isFinite(delta) || delta === 0) return
        if (action === "zoom")
            root.zoomAt(x, Math.pow(1.15, -delta / 120))
        else if (action === "scroll_horizontal")
            root.scrollByPixels(-delta * 2 / 3)
    }

    function scrollByPixels(pixels) {
        root.autoFit = false
        root.viewStart += pixels * root.framesPerPixel
        root.clampView()
    }

    function zoomToFit() {
        if (root.viewWidth <= 0 || root.totalFrames <= 0)
            return
        root.autoFit = true
        root.viewStart = 0
        root.framesPerPixel = root._maxFpp()
        root.clampView()
    }

    function zoomToRange(start, length) {
        if (root.viewWidth <= 0 || length <= 0)
            return
        root.autoFit = false
        // A little margin either side, so the range's edges are visible rather than
        // flush against the frame.
        root.framesPerPixel = (length * 1.1) / root.viewWidth
        root.viewStart = start - (length * 0.05)
        root.clampView()
    }

    // A newly loaded take starts fully zoomed out, and stays fitted while the window
    // is resized -- until the user zooms or scrolls, which is what takes control away.
    //
    // "Fit once per take" was not enough: a tiling compositor resizes the window
    // several times while laying it out, and one transient bad width was enough to
    // clamp the view to a single frame per pixel with no way back.
    property bool autoFit: true

    function maybeFit() {
        if (root.totalFrames > 0 && root.viewWidth > 0 && root.autoFit) {
            root.zoomToFit()
            return true
        }
        return false
    }

    // Watching our own properties rather than anyone else's signals: a Connections
    // block on the service silently never fired in Phase 2, and these cannot miss.
    onViewWidthChanged: {
        if (root.viewWidth <= 0)
            return
        if (!root.maybeFit())
            root.clampView()
    }

    onTotalFramesChanged: root.maybeFit()
}
