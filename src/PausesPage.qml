pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import QtCore
import "ui"

// Tidy pauses: a page of the Sound tab. Finds the gaps between words
// (Backend::findPauses), shades them on the lanes, and then shortens, removes
// or marks them, or splits the clips at them (Backend::applyPauses), as one
// Undo step. Nothing changes until the button at the bottom is pressed; a
// shaded pause clicked on the waveform is kept as it is.
//
// The page lives in the Sound tab, which is made again whenever it is shown,
// so whether it is open is the window's (shell.tidyPausesOpen) and what was
// chosen here is remembered between sessions.
Item {
    id: root
    objectName: "pausesPage"

    property var shell: null
    readonly property var service: shell ? shell.service : null

    // ── choices ──────────────────────────────────────────────────────────
    property string scope: "voices"
    property bool autoLevel: true
    property real thresholdDb: -44
    property real minSeconds: 0.5
    property real paddingSeconds: 0.1
    property bool keepBreaths: true
    property int thenIndex: 0
    property real shortenTo: 0.4
    property bool moveOthers: true
    property real smoothMs: 10
    property string markAs: "marker"
    property string message: ""
    readonly property var thenKeys: ["shorten", "remove", "mark", "split"]
    readonly property string then: thenKeys[Math.max(0, Math.min(3, thenIndex))]
    readonly property bool changesTiming: then === "shorten" || then === "remove"

    Settings {
        category: "pauses"
        property alias autoLevel: root.autoLevel
        property alias thresholdDb: root.thresholdDb
        property alias minSeconds: root.minSeconds
        property alias paddingSeconds: root.paddingSeconds
        property alias keepBreaths: root.keepBreaths
        property alias thenIndex: root.thenIndex
        property alias shortenTo: root.shortenTo
        property alias moveOthers: root.moveOthers
        property alias smoothMs: root.smoothMs
        property alias markAs: root.markAs
    }

    // Opened from Edit › Tidy Pauses… with a selection, it looks in the selection.
    Component.onCompleted: if (shell && shell.tidyPausesScope) scope = shell.tidyPausesScope
    Connections {
        target: root.shell
        function onTidyPausesScopeChanged() { if (root.shell.tidyPausesScope) root.scope = root.shell.tidyPausesScope }
    }

    // ── what the backend holds ───────────────────────────────────────────
    readonly property var pauses: service ? service.pauses : ({})
    readonly property var result: service ? service.pauseResult : ({})
    readonly property bool found: pauses.valid === true
    readonly property bool stale: found && pauses.stale === true
    readonly property bool finished: result.then !== undefined && result.then !== ""
    readonly property int rate: service && service.sampleRate > 0 ? service.sampleRate : 48000

    function spec() {
        return { scope: scope, auto: autoLevel, thresholdDb: Math.round(thresholdDb), minSeconds: minSeconds,
                 paddingSeconds: paddingSeconds, keepBreaths: keepBreaths }
    }
    function action() {
        return { then: then, shortenTo: shortenTo, moveOthers: moveOthers, smoothMs: Math.round(smoothMs), markAs: markAs }
    }
    // What the chosen action would do, read again when the pauses or a choice change.
    readonly property var plan: {
        if (!service || !found || stale) return ({ valid: false })
        void pauses
        return service.pausePlan({ then: then, shortenTo: shortenTo, moveOthers: moveOthers,
                                   smoothMs: Math.round(smoothMs), markAs: markAs })
    }
    readonly property int changes: plan.valid === true ? (plan.changes || 0) : 0
    // The lanes shade amber what this will change, as the backend plans it. Once
    // the pauses are stale the last plan stays, dimmed, until they are found again.
    property var shownPlan: ({ changed: [], lengths: [] })
    onPlanChanged: if (plan.valid === true || !found) shownPlan = plan.valid === true ? plan : ({ changed: [], lengths: [] })
    Binding {
        target: root.shell
        property: "tidyPausesAction"
        value: ({ then: root.then, changed: root.shownPlan.changed || [], lengths: root.shownPlan.lengths || [] })
        when: root.shell !== null
    }

    function find() {
        if (!service) return
        stopJoins()
        message = ""
        const r = service.findPauses(spec())
        if (!r || r.valid !== true) message = (r && r.reason) || service.lastError
        joinIndex = 0
        revealTimer.restart()
    }
    // The card with what was found, scrolled into view under the button once
    // the page has made room for it.
    Timer {
        id: revealTimer
        interval: 60
        onTriggered: flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }
    function clear() {
        stopJoins()
        message = ""
        if (service) service.clearPauses()
    }
    // What was kept, so Undo can show the same pauses again.
    property var keptBefore: []
    property var specBefore: ({})
    function apply() {
        if (!service) return
        stopJoins()
        message = ""
        keptBefore = (pauses.pauses || []).filter(p => p.kept).map(p => p.start)
        specBefore = spec()
        if (!service.applyPauses(action())) { message = service.lastError; return }
        joinIndex = 0
    }
    // Undo puts the audio back and finds the same pauses again, kept as they were.
    function undo() {
        if (!service) return
        stopJoins()
        service.undo()
        service.clearPauseResult()
        const r = service.findPauses(Object.keys(specBefore).length ? specBefore : spec())
        if (r && r.valid === true)
            for (const p of r.pauses || [])
                if (keptBefore.indexOf(p.start) >= 0) service.setPauseKept(p.index, true)
        joinIndex = 0
    }
    function done() {
        stopJoins()
        if (service) service.clearPauseResult()
    }
    function back() {
        stopJoins()
        if (service) { service.clearPauses(); service.clearPauseResult() }
        if (shell) shell.tidyPausesOpen = false
    }

    // ── words ────────────────────────────────────────────────────────────
    function seconds(s) {
        s = Math.max(0, Number(s || 0))
        if (s < 60) return qsTr("%1 s").arg(s < 10 ? s.toFixed(1) : Math.round(s))
        const m = Math.floor(s / 60)
        return qsTr("%1 min %2 s").arg(m).arg(Math.round(s - m * 60))
    }
    function pauseCount(n) { return n === 1 ? qsTr("1 pause") : qsTr("%1 pauses").arg(n) }
    function length(frames) { return service ? service.formatDuration(Math.round(frames || 0)) : "" }
    function db(v) { return qsTr("%1 dB").arg(String(Math.round(v)).replace("-", "−")) }
    readonly property string applyLabel: {
        if (changes === 0) {
            switch (then) {
            case "remove": return qsTr("Remove pauses")
            case "mark": return qsTr("Mark pauses")
            case "split": return qsTr("Split at pauses")
            default: return qsTr("Shorten pauses")
            }
        }
        switch (then) {
        case "remove": return changes === 1 ? qsTr("Remove 1 pause") : qsTr("Remove %1 pauses").arg(changes)
        case "mark": return changes === 1 ? qsTr("Mark 1 pause") : qsTr("Mark %1 pauses").arg(changes)
        case "split": return changes === 1 ? qsTr("Split at 1 pause") : qsTr("Split at %1 pauses").arg(changes)
        default: return changes === 1 ? qsTr("Shorten 1 pause") : qsTr("Shorten %1 pauses").arg(changes)
        }
    }

    // What "Look in" covers: before a find, from the project; after, from the find.
    readonly property string targetText: {
        if (!service) return ""
        void service.tracksRevision; void service.selectionStart; void service.selectionEnd; void service.focusedTrack
        const voices = ["dialogue", "host", "guest"]
        const all = service.tracks || []
        // Each name cut to fit, and more than three tracks counted, so the line stays short.
        const names = all0 => {
            const list = all0.map(n => Theme.shortName(n, 32))
            return list.length === 0 ? "" : list.length === 1 ? list[0]
                : list.length === 2 ? qsTr("%1 and %2").arg(list[0]).arg(list[1])
                : list.length > 3 ? qsTr("%1 voice tracks").arg(list.length)
                : qsTr("%1 and %2").arg(list.slice(0, -1).join(", ")).arg(list[list.length - 1])
        }
        if (found && pauses.scope === scope) {
            const range = pauses.scope === "selection" ? qsTr("the selection (%1)").arg(length(pauses.end - pauses.start))
                        : pauses.scope === "track" ? qsTr("the whole track (%1)").arg(length(pauses.end - pauses.start))
                        : qsTr("the whole project (%1)").arg(length(pauses.end - pauses.start))
            return qsTr("%1, %2").arg(names(pauses.trackNames || [])).arg(range)
        }
        const focused = all.find(t => t.id === service.focusedTrack)
        if (scope === "selection") {
            if (service.selectionEmpty) return qsTr("Select a stretch of a track first.")
            return focused ? qsTr("%1, the selection (%2)").arg(Theme.shortName(focused.name, 40)).arg(length(service.selectionEnd - service.selectionStart))
                           : qsTr("Click a track first.")
        }
        if (scope === "track")
            return focused ? qsTr("%1, the whole track (%2)").arg(Theme.shortName(focused.name, 40)).arg(length(focused.frames)) : qsTr("Click a track first.")
        const v = all.filter(t => voices.indexOf(t.role) >= 0)
        if (v.length === 0) return qsTr("There is no voice track. Set a track's kind to Voice in the Sound tab.")
        return qsTr("%1, the whole project (%2)").arg(names(v.map(t => t.name))).arg(length(service.takeFrames))
    }

    // ── playing the joins ────────────────────────────────────────────────
    // Before Shorten or Remove, the places the joins will be, as they sound
    // now; after, the new joins. Undo goes back to compare.
    readonly property var joins: {
        if (finished) return result.joins || []
        if (!found || stale) return []
        if (plan.valid === true && (plan.joins || []).length) return plan.joins
        return (pauses.pauses || []).filter(p => !p.kept).map(p => Math.round((p.start + p.end) / 2))
    }
    property int joinIndex: 0
    readonly property int shownJoin: Math.max(0, Math.min(joins.length - 1, joinIndex))
    property bool playingJoins: false
    readonly property bool servicePlaying: service ? service.playing : false
    onServicePlayingChanged: if (!servicePlaying && playingJoins && !joinTimer.starting) stopJoins(true)
    function playJoin(i) {
        if (!service || joins.length === 0) return
        joinIndex = Math.max(0, Math.min(joins.length - 1, i))
        joinTimer.starting = true
        service.seekFrames(Math.max(0, Math.round(joins[joinIndex] - 2 * rate)))
        if (!service.playing) service.play()
        joinTimer.starting = false
        playingJoins = true
        joinTimer.restart()
    }
    function stopJoins(already) {
        if (!playingJoins) return
        playingJoins = false
        joinTimer.stop()
        if (!already && service && service.playing) service.stop()
    }
    function step(by) {
        const next = Math.max(0, Math.min(joins.length - 1, shownJoin + by))
        if (playingJoins) playJoin(next)
        else {
            joinIndex = next
            if (service && joins.length) service.seekFrames(Math.max(0, Math.round(joins[next] - 2 * rate)))
        }
    }
    Timer {
        id: joinTimer
        objectName: "pausesJoinTimer"
        property bool starting: false
        interval: 4000
        onTriggered: {
            if (root.shownJoin + 1 < root.joins.length) root.playJoin(root.shownJoin + 1)
            else root.stopJoins()
        }
    }
    onJoinsChanged: if (joinIndex >= joins.length) joinIndex = 0
    Component.onDestruction: stopJoins()

    // ── small parts ──────────────────────────────────────────────────────
    component Label: Text {
        textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        color: Theme.textPrimary
    }
    component Hint: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    component Heading: Text {
        textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading
        color: Theme.textPrimary
    }
    component Rule: Rectangle { width: parent ? parent.width : 0; height: Theme.hairline; color: Theme.border }
    // A settings line: the name on the left, the control on the right, a hint under.
    component Field: Column {
        id: field
        property string label: ""
        property string hint: ""
        default property alias control: holder.data
        width: parent ? parent.width : 0
        spacing: Theme.s1
        Item {
            width: parent.width
            height: Theme.hControl
            Label { anchors.verticalCenter: parent.verticalCenter; text: field.label }
            Row {
                id: holder
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2
            }
        }
        Hint { visible: field.hint !== ""; text: field.hint }
    }
    // One of several, as a round mark and a line of words.
    component Choice: C.AbstractButton {
        id: choice
        property string helpText: ""
        width: parent ? parent.width : 0
        implicitHeight: Theme.hRow
        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        checkable: false
        Accessible.role: Accessible.RadioButton
        Accessible.name: text
        Accessible.checked: checked
        background: Rectangle {
            radius: Theme.rControl
            color: choice.hovered && !choice.checked ? Theme.hoverOverlay : "transparent"
        }
        contentItem: Row {
            spacing: Theme.s2
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 14; height: 14; radius: 7
                color: "transparent"
                border.width: choice.visualFocus ? Theme.focusRing : Theme.hairline
                border.color: choice.checked || choice.visualFocus ? Theme.accent : choice.hovered ? Theme.borderStrong : Theme.border
                Rectangle {
                    anchors.centerIn: parent
                    visible: choice.checked
                    width: 6; height: 6; radius: 3
                    color: Theme.accent
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: choice.text
                textFormat: Text.PlainText
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                color: choice.checked ? Theme.textPrimary : Theme.textSecondary
            }
        }
        ControlHelp { title: choice.text; purpose: choice.helpText }
    }
    // ‹ Join 4 of 35 › and Play the joins, before and after.
    component JoinPlayer: Column {
        id: player
        property string note: ""
        width: parent ? parent.width : 0
        spacing: Theme.s2
        visible: root.joins.length > 0
        Item {
            width: parent.width
            height: Theme.hControl
            IconButton {
                id: playButton
                objectName: "pausesPlayJoins"
                anchors.verticalCenter: parent.verticalCenter
                iconName: root.playingJoins ? "square" : "play"
                text: root.playingJoins ? qsTr("Stop") : qsTr("Play the joins")
                flat: false
                enabled: root.joins.length > 0
                helpText: qsTr("Plays 2 s either side of each join in turn, then moves to the next.")
                helpScope: qsTr("Before the change it plays the pauses as they are now; after, the new joins. Undo to compare.")
                onClicked: root.playingJoins ? root.stopJoins() : root.playJoin(root.shownJoin)
            }
            Label {
                anchors.left: playButton.right; anchors.leftMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Play the joins")
            }
        }
        Item {
            width: parent.width
            height: Theme.hRow
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1
                IconButton {
                    objectName: "pausesJoinPrevious"
                    iconName: "chevron-left"
                    text: qsTr("Previous join")
                    enabled: root.shownJoin > 0
                    helpText: qsTr("Go to the join before this one.")
                    onClicked: root.step(-1)
                }
                Text {
                    objectName: "pausesJoinLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Join %1 of %2").arg(root.shownJoin + 1).arg(root.joins.length)
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                    color: Theme.textSecondary
                }
                IconButton {
                    objectName: "pausesJoinNext"
                    iconName: "chevron-right"
                    text: qsTr("Next join")
                    enabled: root.shownJoin + 1 < root.joins.length
                    helpText: qsTr("Go to the join after this one.")
                    onClicked: root.step(1)
                }
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.service && root.joins.length ? qsTr("at %1").arg(root.service.formatTime(Math.round(root.joins[root.shownJoin]))) : ""
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                color: Theme.textMuted
            }
        }
        Hint { text: player.note }
    }

    // ── the page ─────────────────────────────────────────────────────────
    Flickable {
        id: flick
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        contentHeight: body.implicitHeight + Theme.s3 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: body
            x: Theme.s3; y: Theme.s3
            width: flick.width - Theme.s3 * 2
            spacing: Theme.s3

            RecipeLink {
                objectName: "pausesBack"
                text: qsTr("‹ Sound")
                chevron: false
                helpText: qsTr("Back to the Sound settings. The shading goes; nothing in the project changes.")
                onClicked: root.back()
            }
            Column {
                width: parent.width
                spacing: Theme.s1
                Text {
                    text: qsTr("Tidy pauses")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                Hint { text: qsTr("Find the gaps between words, then shorten, remove or mark them.") }
            }

            // ── the finished card ────────────────────────────────────────
            Rectangle {
                objectName: "pausesDoneCard"
                visible: root.finished
                width: parent.width
                height: doneCol.implicitHeight + Theme.s3 * 2
                color: Theme.panelRaised
                radius: Theme.rMenu
                border.width: Theme.hairline
                border.color: Theme.border
                Column {
                    id: doneCol
                    x: Theme.s3; y: Theme.s3
                    width: parent.width - Theme.s3 * 2
                    spacing: Theme.s2
                    Item {
                        width: parent.width
                        height: Theme.hRow
                        Heading {
                            objectName: "pausesDoneTitle"
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.result.then === "remove" ? qsTr("Pauses removed")
                                : root.result.then === "mark" ? qsTr("Pauses marked")
                                : root.result.then === "split" ? qsTr("Split at the pauses")
                                : qsTr("Pauses shortened")
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("%1 of %2").arg(root.result.changed || 0).arg(root.result.total || 0)
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                            color: Theme.textMuted
                        }
                    }
                    Rule {}
                    Repeater {
                        model: {
                            const r = root.result
                            const rows = []
                            const timing = r.then === "shorten" || r.then === "remove"
                            if (timing)
                                rows.push({ name: qsTr("Length"), value: qsTr("%1 → %2  (−%3)").arg(root.length(r.lengthBefore))
                                            .arg(root.length(r.lengthAfter)).arg(root.seconds(r.removedSeconds)) })
                            rows.push({ name: qsTr("Kept as they were"), value: root.pauseCount(r.kept || 0) })
                            if (timing) {
                                rows.push({ name: qsTr("Joins"), value: (r.smoothMs || 0) > 0
                                            ? qsTr("%1, each with a %2 ms crossfade").arg((r.joins || []).length).arg(Math.round(r.smoothMs))
                                            : qsTr("%1, no crossfade").arg((r.joins || []).length) })
                                rows.push({ name: qsTr("Tracks moved"), value: String(r.moved || 0) })
                            } else if (r.then === "mark") {
                                rows.push({ name: qsTr("Marked"), value: qsTr("%1, in the Markers list").arg(r.changed || 0) })
                            } else {
                                rows.push({ name: qsTr("Split"), value: qsTr("at %1").arg(root.pauseCount(r.changed || 0)) })
                            }
                            return rows
                        }
                        Item {
                            id: doneRow
                            required property var modelData
                            width: doneCol.width
                            height: Theme.hRow
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: doneRow.modelData.name
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                color: Theme.textSecondary
                            }
                            Text {
                                objectName: "pausesDoneValue"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: doneRow.modelData.value
                                textFormat: Text.PlainText
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.features: { "tnum": 1 }
                                color: Theme.textPrimary
                            }
                        }
                    }
                    Rule {}
                    JoinPlayer {
                        note: qsTr("Plays 2 s either side of each new join in turn. Undo to hear it as it was.")
                    }
                    Hint {
                        text: (root.result.then === "shorten" || root.result.then === "remove"
                               ? qsTr("Markers after each join moved with the audio. ") : "")
                              + qsTr("Undo takes it all back in one step (“%1”).").arg(root.result.undoLabel || "")
                    }
                    Item {
                        width: parent.width
                        height: Theme.hControl
                        Row {
                            anchors.right: parent.right
                            spacing: Theme.s2
                            ToolButton {
                                objectName: "pausesUndo"
                                text: qsTr("Undo")
                                enabled: root.service !== null && root.service.canUndo
                                helpText: qsTr("Put the audio back as it was and show the same pauses again, to compare.")
                                onClicked: root.undo()
                            }
                            ToolButton {
                                objectName: "pausesDone"
                                text: qsTr("Done")
                                prominent: true
                                helpText: qsTr("Keep the change and close this card.")
                                onClicked: root.done()
                            }
                        }
                    }
                }
            }

            // ── the settings ─────────────────────────────────────────────
            Column {
                visible: !root.finished
                width: parent.width
                spacing: Theme.s3

                Column {
                    width: parent.width
                    spacing: Theme.s2
                    Label { height: Theme.hRow; verticalAlignment: Text.AlignVCenter; text: qsTr("Look in") }
                    SegmentedControl {
                        objectName: "pausesScope"
                        readonly property var keys: ["selection", "track", "voices"]
                        labels: [qsTr("Selection"), qsTr("This track"), qsTr("Every voice track")]
                        currentIndex: keys.indexOf(root.scope)
                        helpTitle: qsTr("Look in")
                        helpText: qsTr("Selection: only the selected stretch of the active track. This track: the whole active track. Every voice track: every Voice track over the whole project; a pause is where all of them are quiet at once, so an answer is never cut.")
                        onActivated: function (i) { root.scope = keys[i] }
                    }
                    Hint {
                        objectName: "pausesTarget"
                        text: root.targetText
                    }
                }
                Rule {}

                Heading { text: qsTr("What counts as a pause") }
                Field {
                    label: qsTr("Quieter than")
                    hint: {
                        if (!root.autoLevel) return qsTr("Quieter than this level counts as silence.")
                        if (root.found && root.pauses.auto === true && root.pauses.noiseFloorDb !== undefined)
                            return qsTr("Auto: %1 dB above the background noise (%2).")
                                .arg(Math.round(root.pauses.thresholdDb - root.pauses.noiseFloorDb)).arg(root.db(root.pauses.noiseFloorDb))
                        return qsTr("Auto: set from the background noise when you find the pauses.")
                    }
                    Toggle {
                        id: autoToggle
                        objectName: "pausesAuto"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.autoLevel
                        helpTitle: qsTr("Auto level")
                        helpText: qsTr("Measure the background noise and count anything a little above it as quiet. Turn off to set the level yourself.")
                        onToggled: {
                            // Turning Auto off starts from the level it last found.
                            if (!checked && root.found && root.pauses.auto === true) root.thresholdDb = Math.round(root.pauses.thresholdDb)
                            root.autoLevel = checked
                            checked = Qt.binding(() => root.autoLevel)
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Auto")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                        color: Theme.textSecondary
                    }
                    ValueField {
                        objectName: "pausesLevel"
                        width: 76
                        from: -90; to: -10; step: 0.5; decimals: 0; suffix: "dB"
                        value: root.autoLevel && root.found && root.pauses.auto === true ? root.pauses.thresholdDb : root.thresholdDb
                        editable: !root.autoLevel
                        helpTitle: qsTr("Quieter than")
                        helpText: qsTr("Below this level counts as quiet. With Auto on it shows the level the last find used.")
                        onEdited: function (v) { root.thresholdDb = v }
                    }
                }
                Field {
                    label: qsTr("For at least")
                    hint: qsTr("Shorter gaps are part of the speech and are left alone.")
                    ValueField {
                        objectName: "pausesMinimum"
                        width: 76
                        from: 0.1; to: 10; step: 0.01; decimals: 2; suffix: "s"
                        value: root.minSeconds
                        helpTitle: qsTr("For at least")
                        helpText: qsTr("How long a quiet gap must last to count as a pause.")
                        onEdited: function (v) { root.minSeconds = v }
                    }
                }
                Field {
                    label: qsTr("Leave each side")
                    hint: qsTr("A little air before and after the words, so nothing is clipped.")
                    ValueField {
                        objectName: "pausesPadding"
                        width: 76
                        from: 0; to: 1; step: 0.005; decimals: 2; suffix: "s"
                        value: root.paddingSeconds
                        helpTitle: qsTr("Leave each side")
                        helpText: qsTr("How much of the quiet to leave next to the words on each side of a pause.")
                        onEdited: function (v) { root.paddingSeconds = v }
                    }
                }
                Field {
                    label: qsTr("Keep breaths")
                    hint: qsTr("A breath in a gap stays, and the pause is shortened around it.")
                    Toggle {
                        objectName: "pausesKeepBreaths"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.keepBreaths
                        helpTitle: qsTr("Keep breaths")
                        helpText: qsTr("A breath in a gap stays, and the pause is shortened around it. Turn off to treat breaths as part of the pause.")
                        onToggled: { root.keepBreaths = checked; checked = Qt.binding(() => root.keepBreaths) }
                    }
                }
                Rule {}

                Heading { text: qsTr("Then") }
                SegmentedControl {
                    objectName: "pausesThen"
                    labels: [qsTr("Shorten"), qsTr("Remove"), qsTr("Mark"), qsTr("Split")]
                    currentIndex: root.thenIndex
                    helpTitle: qsTr("Then")
                    helpText: qsTr("What to do with the pauses found: shorten them, remove them, mark them, or split the clips at them.")
                    onActivated: function (i) { root.thenIndex = i }
                }
                Field {
                    visible: root.then === "shorten"
                    label: qsTr("Shorten each to")
                    hint: qsTr("Keeps the rhythm of the talk. Pauses already this short are left.")
                    ValueField {
                        objectName: "pausesShortenTo"
                        width: 76
                        from: 0.05; to: 5; step: 0.01; decimals: 2; suffix: "s"
                        value: root.shortenTo
                        helpTitle: qsTr("Shorten each to")
                        helpText: qsTr("How long each pause becomes, counting the air left each side.")
                        onEdited: function (v) { root.shortenTo = v }
                    }
                }
                Hint {
                    visible: root.then === "remove"
                    text: qsTr("Remove: only the air each side is left (%1).").arg(root.seconds(root.paddingSeconds * 2).replace(".0 s", " s"))
                          + (root.plan.valid === true ? "  " + qsTr("%1 → %2.").arg(root.length(root.plan.lengthBefore)).arg(root.length(root.plan.lengthAfter)) : "")
                }
                Column {
                    visible: root.then === "mark"
                    width: parent.width
                    spacing: Theme.s1
                    Repeater {
                        model: [{ key: "marker", label: qsTr("A marker at each pause"), help: qsTr("One marker in the middle of each pause.") },
                                { key: "pause", label: qsTr("A range over each pause"), help: qsTr("A range marker covering each pause, from the end of one word to the start of the next.") },
                                { key: "speech", label: qsTr("A range over each stretch of speech"), help: qsTr("A range marker covering each stretch of speech between the pauses.") }]
                        Choice {
                            required property var modelData
                            objectName: "pausesMarkAs_" + modelData.key
                            text: modelData.label
                            checked: root.markAs === modelData.key
                            helpText: modelData.help
                            onClicked: root.markAs = modelData.key
                        }
                    }
                    Hint { text: qsTr("Nothing is cut. Named “Pause 1”, “Pause 2”… in the Markers list.") }
                }
                Hint {
                    visible: root.then === "split"
                    text: qsTr("Split at pauses: each stretch of speech becomes its own clip and the pauses are left empty. The timing does not change.")
                }
                Column {
                    visible: root.changesTiming
                    width: parent.width
                    spacing: Theme.s1
                    Label { height: Theme.hRow; verticalAlignment: Text.AlignVCenter; text: qsTr("Other tracks") }
                    Choice {
                        objectName: "pausesOthers_move"
                        text: qsTr("Move the other tracks too")
                        checked: root.moveOthers
                        helpText: qsTr("Effects, ambience and the other tracks that are not music move with the voices, so they stay in step. Music never moves.")
                        onClicked: root.moveOthers = true
                    }
                    Choice {
                        objectName: "pausesOthers_only"
                        text: qsTr("Only the tracks looked in")
                        checked: !root.moveOthers
                        helpText: qsTr("Only the tracks looked in change; every other track stays where it is.")
                        onClicked: root.moveOthers = false
                    }
                    Hint {
                        text: root.moveOthers
                              ? qsTr("Effects, ambience and markers move with the voices and stay in step. Music tracks never move.")
                              : qsTr("The other tracks stay where they are. Music tracks never move.")
                    }
                }
                Field {
                    visible: root.changesTiming
                    label: qsTr("Smooth each join")
                    ValueField {
                        objectName: "pausesSmooth"
                        width: 76
                        from: 0; to: 100; step: 0.5; decimals: 0; suffix: "ms"
                        value: root.smoothMs
                        helpTitle: qsTr("Smooth each join")
                        helpText: qsTr("A short crossfade at each join, so it does not click. 0 ms is a plain cut.")
                        onEdited: function (v) { root.smoothMs = v }
                    }
                }
                ToolButton {
                    objectName: "pausesFind"
                    text: root.found ? qsTr("Find pauses again") : qsTr("Find pauses")
                    iconName: "search"; showLabel: true
                    prominent: !root.found
                    helpText: qsTr("Look for the pauses with these settings and shade them on the waveform. Nothing changes yet.")
                    helpScope: qsTr("Finding again forgets which pauses were kept.")
                    onClicked: root.find()
                }

                // ── what was found ───────────────────────────────────────
                Rectangle {
                    objectName: "pausesFoundCard"
                    visible: root.found
                    width: parent.width
                    height: foundCol.implicitHeight + Theme.s3 * 2
                    color: Theme.panelRaised
                    radius: Theme.rMenu
                    border.width: Theme.hairline
                    border.color: Theme.withAlpha(Theme.maskOutline, 0.45)
                    Column {
                        id: foundCol
                        x: Theme.s3; y: Theme.s3
                        width: parent.width - Theme.s3 * 2
                        spacing: Theme.s2
                        Item {
                            width: parent.width
                            height: Theme.hRow
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 9; height: 9; radius: 2
                                color: Theme.maskOutline
                            }
                            Text {
                                objectName: "pausesCount"
                                x: 16
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("%1 · %2").arg(root.pauseCount(root.pauses.count || 0)).arg(root.seconds(root.pauses.seconds))
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle - 1; font.weight: Theme.wHeading
                                font.features: { "tnum": 1 }
                                color: Theme.textPrimary
                            }
                            Text {
                                objectName: "pausesKept"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                visible: (root.pauses.kept || 0) > 0
                                text: qsTr("%1 kept").arg(root.pauses.kept || 0)
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                color: Theme.maskKeepOutline
                            }
                        }
                        Text {
                            objectName: "pausesStale"
                            visible: root.stale
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("The project changed. Find pauses again.")
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                            color: Theme.warning
                        }
                        Item {
                            objectName: "pausesLength"
                            visible: !root.stale && root.plan.valid === true
                            width: parent.width
                            height: Theme.hRow
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Length")
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                color: Theme.textSecondary
                            }
                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.s1
                                readonly property bool shorter: (root.plan.lengthAfter || 0) < (root.plan.lengthBefore || 0)
                                Text {
                                    text: root.length(root.plan.lengthBefore)
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.features: { "tnum": 1 }
                                    color: Theme.textSecondary
                                }
                                Text {
                                    visible: parent.shorter
                                    text: "→"
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                                    color: Theme.textMuted
                                }
                                Text {
                                    visible: parent.shorter
                                    text: root.length(root.plan.lengthAfter)
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Theme.wHeading; font.features: { "tnum": 1 }
                                    color: Theme.textPrimary
                                }
                                Text {
                                    text: parent.shorter ? qsTr("(−%1)").arg(root.seconds(root.plan.removedSeconds)) : qsTr("(unchanged)")
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                    color: Theme.textMuted
                                }
                            }
                        }
                        Rule { visible: root.joins.length > 0 }
                        JoinPlayer {
                            note: qsTr("Plays 2 s either side of each pause in turn, as it sounds now. Click a shaded pause to keep it.")
                        }
                        Hint {
                            visible: !root.stale && (root.pauses.count || 0) === 0
                            text: qsTr("No pauses this long. Try a shorter For at least, or a higher Quieter than level.")
                        }
                    }
                }

                Text {
                    objectName: "pausesMessage"
                    visible: root.message !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.message
                    textFormat: Text.PlainText
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.danger
                }

            }
        }
    }
    Rectangle {
        id: footer
        objectName: "pausesFooter"
        visible: root.found && !root.finished
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: visible ? Theme.hControl + Theme.s3 * 2 : 0
        color: Theme.panelBg
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Item {
            x: Theme.s3; y: Theme.s3
            width: parent.width - Theme.s3 * 2
            height: Theme.hControl
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("One Undo step.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.s2
                ToolButton {
                    objectName: "pausesClear"
                    text: qsTr("Clear")
                    helpText: qsTr("Forget the pauses found and take the shading away. Nothing changes.")
                    onClicked: root.clear()
                }
                ToolButton {
                    objectName: "pausesApply"
                    text: root.applyLabel
                    prominent: true
                    enabled: !root.stale && root.plan.valid === true && root.changes > 0
                    helpText: qsTr("Do it now, to every pause shaded amber. Kept pauses stay as they are. One Undo step.")
                    unavailableReason: root.stale ? qsTr("The project changed. Find pauses again.")
                                     : root.changes === 0 ? qsTr("Every pause is kept, or none was found.") : ""
                    onClicked: root.apply()
                }
            }
        }
    }
}
