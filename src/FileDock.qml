pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The left dock: Files, Markers, History. Files is the open take, its facts,
// the tracks in the session, and the two ways in. Markers is the session's
// marker list -- searchable, click to go, double-click to rename. History is
// the undo stack as a list a person can click back through.
Rectangle {
    id: root
    objectName: "fileDock"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int currentTab: 0

    color: Theme.panelBg

    Rectangle {
        anchors.right: parent.right
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
        anchors.right: parent.right
        tabs: [qsTr("Files"), qsTr("Markers"), qsTr("History")]
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        trailing: [
            IconButton {
                iconName: "panel-left"; text: qsTr("Hide Files")
                helpText: qsTr("Hide this dock. Show it again from the View menu.")
                onClicked: if (root.shell) root.shell.leftDockVisible = false
            }
        ]
    }

    // ── Files ───────────────────────────────────────────────────────────
    Item {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        visible: root.currentTab === 0

        Column {
            anchors.fill: parent
            anchors.margins: Theme.s2
            spacing: Theme.s2

            Row {
                width: parent.width
                spacing: Theme.s1
                ToolButton {
                    objectName: "fileDockOpen"
                    iconName: "folder-open"; text: qsTr("Open Audio…")
                    helpText: qsTr("Open an audio file as a new project.")
                    helpScope: qsTr("Replaces the current project. You are asked to save unsaved changes first.")
                    showLabel: true
                    shortcut: root.shell ? root.shell.bindingFor("open_file") : ""
                    onClicked: root.shell.runAction("open_file")
                }
                ToolButton {
                    iconName: "file-plus"; text: qsTr("Import as Track…")
                    helpText: qsTr("Add an audio file to the current project as a new track.")
                    helpScope: qsTr("The project is kept. Audio at another sample rate is converted to the project rate.")
                    showLabel: true
                    shortcut: root.shell ? root.shell.bindingFor("import_file") : ""
                    onClicked: root.shell.runAction("import_file")
                }
            }

            ToolButton {
                objectName: "importSessionButton"; text: qsTr("Import Session (AAF/OMF)…"); showLabel: true
                helpText: qsTr("Bring an AAF or OMF audio timeline into this project, keeping its tracks, clip positions and source offsets.")
                helpScope: qsTr("Use Resolve’s Pro Tools AAF export, or an OMF 2.0 export with embedded or separate audio. The import panel offers an audio folder and reports anything that cannot transfer. One Undo removes the import.")
                onClicked: if (root.shell) root.shell.runAction("import_session")
            }

            ComboField {
                objectName: "importPlacement"
                width: parent.width
                caption: qsTr("Import at")
                helpTitle: qsTr("Import placement")
                helpText: qsTr("Where an imported track lands: the start of the timeline, the playhead, or the time the file says it was recorded.")
                helpScope: qsTr("Recorded time uses the file's broadcast time reference against the project's. The first such import into a project without a reference sets the project's reference and lands at the start.")
                helpTopic: qsTr("A file without a recorded time is placed at the start and the status bar says so. Opening a file as a new project always starts at 0. Change the project's reference in the Metadata panel.")
                model: [qsTr("Start"), qsTr("Playhead"), qsTr("Recorded time")]
                currentIndex: root.shell ? ["start", "playhead", "timestamp"].indexOf(root.shell.importPlacement) : 0
                onActivated: index => { if (root.shell) root.shell.importPlacement = ["start", "playhead", "timestamp"][index] }
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            ToolButton {
                objectName: "recentDocumentsButton"; text: qsTr("Recent documents…"); showLabel: true
                helpText: qsTr("List recently saved projects and opened files, to reopen one or import its audio as a track.")
                shortcut: root.shell ? root.shell.bindingFor("show_recent_documents") : ""
                onClicked: root.shell.runAction("show_recent_documents")
            }

            Text {
                text: qsTr("PROJECT")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.weight: Theme.wHeading
                font.letterSpacing: 0.4
                color: Theme.textMuted
            }

            // The project, when there is one.
            Row {
                width: parent.width
                spacing: Theme.s2
                visible: root.service !== null && root.service.projectName !== ""
                Icon { name: "folder"; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - Theme.szIcon - Theme.s2
                    text: root.service ? root.service.projectName + (root.service.dirty ? " •" : "") : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Column header, as the concept lays the file list out.
            Row {
                width: fileColumn.width
                visible: root.service !== null && root.service.trackCount > 0
                Text { width: parent.width - 96; text: qsTr("Name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: 60; text: qsTr("Clips"); horizontalAlignment: Text.AlignRight; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: 36; text: qsTr("Ch"); horizontalAlignment: Text.AlignRight; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            }

            // The track list takes what is left of the panel and scrolls: a session
            // of 45 stems runs well past the bottom of a laptop screen.
            Flickable {
                id: fileList
                objectName: "fileList"
                width: parent.width
                height: Math.max(0, parent.height - y - fileCount.height - Theme.s1)
                visible: fileRows.count > 0
                contentHeight: fileColumn.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar { id: fileBar }

                // Up and Down move the focus; the list keeps the focused row in view.
                function revealFocused() {
                    if (!root.service || root.service.focusedTrack < 0)
                        return
                    const top = root.service.trackIndexById(root.service.focusedTrack) * Theme.hRow
                    if (top < contentY)
                        contentY = top
                    else if (top + Theme.hRow > contentY + height)
                        contentY = Math.min(top + Theme.hRow - height, Math.max(0, contentHeight - height))
                }
                Connections {
                    target: root.service
                    function onTracksChanged() { Qt.callLater(fileList.revealFocused) }
                }

                Column {
                    id: fileColumn
                    // Room for the scrollbar, so it never sits over the Ch column.
                    width: fileList.width - fileBar.gutter
                    Repeater {
                        id: fileRows
                        model: root.service ? root.service.trackIdModel : null
                        delegate: Rectangle {
                            id: row
                            objectName: "fileRow"
                            required property var modelData
                            property int revision: 0   // this track's data changed (see MultitrackWorkspace)
                            Connections {
                                target: root.service
                                function onTrackDataChanged(ids) { if (ids.indexOf(row.modelData) >= 0) ++row.revision }
                            }
                            readonly property var track: {
                                row.revision
                                const candidate = root.service ? root.service.trackById(modelData) : null
                                return candidate && candidate.id !== undefined ? candidate : null
                            }
                            readonly property bool focused: root.service !== null && track !== null
                                                            && root.service.focusedTrack === track.id
                            width: fileColumn.width
                            height: Theme.hRow
                            radius: Theme.rControl
                            color: focused ? (rowHover.hovered ? Theme.selectedHover : Theme.selectedFill) : rowHover.hovered ? Theme.hoverOverlay : "transparent"
                            HoverHandler { id: rowHover }
                            TapHandler { onTapped: if (root.service && row.track) root.service.focusTrack(row.track.id) }
                            ControlHelp {
                                title: row.track ? row.track.name : qsTr("Track")
                                purpose: qsTr("A track in this project, with its clip and channel counts. Click to focus it.")
                            }
                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.s1
                                anchors.rightMargin: Theme.s1
                                Row {
                                    width: parent.width - 96
                                    spacing: Theme.s2
                                    anchors.verticalCenter: parent.verticalCenter
                                    Icon { name: "audio-waveform"; color: Theme.audioPrimary; size: 14; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        width: parent.width - 14 - Theme.s2
                                        text: row.track ? row.track.name : ""
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fsControl
                                        color: row.focused ? Theme.textPrimary : Theme.textSecondary
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Text {
                                    width: 60; horizontalAlignment: Text.AlignRight
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: row.track ? row.track.clipCount : ""
                                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                    font.features: { "tnum": 1 }
                                    color: Theme.textMuted
                                }
                                Text {
                                    width: 36; horizontalAlignment: Text.AlignRight
                                    anchors.verticalCenter: parent.verticalCenter
                                    objectName: "fileTrackChannels_" + row.modelData
                                    text: row.track ? row.track.channels : ""
                                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                    font.features: { "tnum": 1 }
                                    color: Theme.textMuted
                                }
                            }
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: 120
                visible: root.service === null || root.service.trackCount === 0
                EmptyState {
                    anchors.fill: parent
                    iconName: "folder-open"
                    title: qsTr("No audio open")
                    description: qsTr("Open a file to see it here, or import one as a new track.")
                }
            }
        }

        Text {
            id: fileCount
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: Theme.s2
            text: !root.service ? ""
                : root.service.trackCount === 1 ? qsTr("1 track")
                : qsTr("%1 tracks").arg(root.service.trackCount)
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    // ── Markers ─────────────────────────────────────────────────────────
    Item {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        visible: root.currentTab === 1

        readonly property var markers: root.service ? root.service.markers : []
        property string filter: ""
        readonly property var shown: {
            const needle = filter.trim().toLowerCase()
            if (needle === "")
                return markers
            var out = []
            for (var i = 0; i < markers.length; ++i) {
                const m = markers[i]
                if (m.name.toLowerCase().indexOf(needle) >= 0
                    || m.notes.toLowerCase().indexOf(needle) >= 0
                    || m.type.indexOf(needle) >= 0)
                    out.push(m)
            }
            return out
        }

        Column {
            id: markerColumn
            anchors.fill: parent
            anchors.margins: Theme.s2
            spacing: Theme.s2

            Row {
                width: parent.width
                spacing: Theme.s1
                SearchField {
                    id: markerSearch
                    objectName: "markerSearch"
                    width: parent.width - addMarkerButton.width - Theme.s1
                    placeholder: qsTr("Search markers…")
                    helpText: qsTr("Type part of a marker's name to show only the markers that match.")
                    helpScope: qsTr("Filters the list only; markers are not changed.")
                    onAccepted: function (t) { markerColumn.parent.filter = t }
                    onCleared: markerColumn.parent.filter = ""
                    // Typing here must not run the keymap; leaving it gives the keys back.
                    onActiveChanged: if (!active && root.shell) root.shell.restoreFocus()
                }
                IconButton {
                    id: addMarkerButton
                    objectName: "markerAdd"
                    iconName: "plus"; text: qsTr("Add marker at playhead")
                    helpText: qsTr("Place a cue marker at the playhead, or a range marker over the selection.")
                    helpScope: qsTr("Markers are saved with the project. Double-click one here to rename it.")
                    unavailableReason: qsTr("Open or import audio first.")
                    shortcut: root.shell ? root.shell.bindingFor("add_marker") : ""
                    enabled: root.service !== null && root.service.loaded
                    onClicked: root.shell.runAction("add_marker")
                }
            }

            // Column header.
            Row {
                width: parent.width
                visible: markerColumn.parent.shown.length > 0
                Text { width: parent.width - 90; text: qsTr("Name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: 90; text: qsTr("Position"); horizontalAlignment: Text.AlignRight; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            }

            Flickable {
                width: parent.width
                height: Math.max(0, parent.height - y - countLine.height - Theme.s2)
                contentHeight: markerRows.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar { id: markerBar }

                Column {
                    id: markerRows
                    width: parent.width - markerBar.gutter
                    spacing: 1

                    Repeater {
                        model: markerColumn.parent.shown
                        delegate: Rectangle {
                            id: mrow
                            objectName: "markerRow"
                            required property var modelData
                            readonly property color tint: modelData.color !== "" ? modelData.color : Theme.markerColor(modelData.type)
                            width: parent.width
                            height: Theme.hRow + (modelData.notes !== "" ? Theme.fsLabel + 2 : 0)
                            radius: Theme.rControl
                            color: mrowHover.hovered ? Theme.hoverOverlay : "transparent"
                            HoverHandler { id: mrowHover }
                            ControlHelp {
                                title: mrow.modelData.name || qsTr("Marker")
                                purpose: qsTr("Click to move the playhead to this marker. Double-click to rename it.")
                            }
                            TapHandler {
                                onTapped: if (root.service) root.service.seekFrames(mrow.modelData.position)
                                onDoubleTapped: {
                                    mrowField.text = mrow.modelData.name
                                    mrowField.visible = true
                                    mrowField.forceActiveFocus()
                                    mrowField.selectAll()
                                }
                            }
                            Row {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Theme.s1
                                anchors.rightMargin: Theme.s1
                                height: Theme.hRow
                                spacing: Theme.s2
                                Icon {
                                    name: Theme.markerIcon(mrow.modelData.type)
                                    size: 13; color: mrow.tint
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Item {
                                    width: parent.width - 13 - Theme.s2 * 3 - 90 - (mrowDelete.visible ? mrowDelete.width : 0)
                                    height: parent.height
                                    Text {
                                        objectName: "markerRowName"
                                        anchors.fill: parent
                                        visible: !mrowField.visible
                                        verticalAlignment: Text.AlignVCenter
                                        text: mrow.modelData.name
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fsControl
                                        color: Theme.textPrimary
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        visible: mrowField.visible
                                        color: Theme.controlBg; radius: Theme.rControl
                                        border.width: Theme.hairline; border.color: Theme.accent
                                    }
                                    TextInput {
                                        id: mrowField
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.s1
                                        visible: false
                                        verticalAlignment: Text.AlignVCenter
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fsControl
                                        selectByMouse: true
                                        selectionColor: Theme.accent
                                        selectedTextColor: Theme.accentText
                                        onAccepted: {
                                            if (root.service) root.service.renameMarker(mrow.modelData.id, text)
                                            visible = false
                                            if (root.shell) root.shell.restoreFocus()
                                        }
                                        Keys.onEscapePressed: { visible = false; if (root.shell) root.shell.restoreFocus() }
                                        onActiveFocusChanged: if (!activeFocus) visible = false
                                    }
                                }
                                Text {
                                    width: 90
                                    horizontalAlignment: Text.AlignRight
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.shell ? root.shell.formatFrames(mrow.modelData.position)
                                                       + (mrow.modelData.length > 0 ? " +" + root.shell.formatDuration(mrow.modelData.length) : "")
                                                     : ""
                                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                    font.features: { "tnum": 1 }
                                    color: Theme.textSecondary
                                }
                                IconButton {
                                    id: mrowDelete
                                    objectName: "markerRowDelete"
                                    iconName: "x"; text: qsTr("Delete marker")
                                    helpText: qsTr("Remove this marker from the project.")
                                    visible: mrowHover.hovered
                                    implicitWidth: Theme.szIcon + 4; implicitHeight: Theme.szIcon + 4
                                    iconColor: hovered ? Theme.danger : Theme.textMuted
                                    anchors.verticalCenter: parent.verticalCenter
                                    onClicked: if (root.service) root.service.removeMarker(mrow.modelData.id)
                                }
                            }
                            Text {
                                visible: mrow.modelData.notes !== ""
                                anchors.left: parent.left
                                anchors.leftMargin: Theme.s1 + 13 + Theme.s2
                                anchors.right: parent.right
                                y: Theme.hRow - 2
                                text: mrow.modelData.notes
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler + 1
                                color: Theme.textMuted
                            }
                        }
                    }
                }
            }

            Text {
                id: countLine
                width: parent.width
                horizontalAlignment: Text.AlignRight
                text: {
                    const n = markerColumn.parent.markers.length
                    if (n === 0) return ""
                    const s = markerColumn.parent.shown.length
                    return s === n ? (n === 1 ? qsTr("1 marker") : qsTr("%1 markers").arg(n))
                                   : qsTr("%1 of %2 markers").arg(s).arg(n)
                }
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }

        EmptyState {
            anchors.fill: parent
            visible: markerColumn.parent.markers.length === 0
            iconName: "bookmark"
            title: qsTr("No markers")
            description: root.shell
                ? qsTr("%1 adds a cue at the playhead, or a range over the selection. Double-click the marker lane to add one there.").arg(root.shell.bindingFor("add_marker"))
                : ""
        }
    }

    // ── History ─────────────────────────────────────────────────────────
    Item {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        visible: root.currentTab === 2

        readonly property var history: root.service ? root.service.history : []
        readonly property int cursor: root.service ? root.service.historyCursor : 0

        Flickable {
            id: historyScroll
            objectName: "historyScroll"
            anchors.fill: parent
            anchors.margins: Theme.s2
            contentHeight: historyRows.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar { id: historyBar }
            visible: parent.history.length > 0

            Column {
                id: historyRows
                width: parent.width - historyBar.gutter
                spacing: 1

                component HistoryRow: Rectangle {
                    id: hrow
                    objectName: "historyRow"
                    property int step: 0
                    property string label: ""
                    readonly property bool applied: step <= historyScroll.parent.cursor
                    readonly property bool current: step === historyScroll.parent.cursor
                    width: parent.width
                    height: Theme.hRow
                    radius: Theme.rControl
                    color: current ? (hrowHover.hovered ? Theme.selectedHover : Theme.selectedFill) : hrowHover.hovered ? Theme.hoverOverlay : "transparent"
                    HoverHandler { id: hrowHover }
                    TapHandler { onTapped: if (root.service) root.service.jumpHistory(hrow.step) }
                    ControlHelp {
                        title: qsTr("History step")
                        purpose: qsTr("Click to take the project back to this point, or forward to it again. It is the same path Undo and Redo walk.")
                    }
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s1
                        anchors.rightMargin: Theme.s1
                        spacing: Theme.s2
                        Icon {
                            name: hrow.step === 0 ? "circle-dot" : hrow.applied ? "circle-check" : "circle-dashed"
                            size: 13
                            color: hrow.current ? Theme.accent : hrow.applied ? Theme.textSecondary : Theme.textMuted
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            width: parent.width - 13 - Theme.s2 * 2 - 30
                            anchors.verticalCenter: parent.verticalCenter
                            text: hrow.label
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsControl
                            font.weight: hrow.current ? Theme.wHeading : Theme.wNormal
                            color: hrow.current ? Theme.textPrimary : hrow.applied ? Theme.textSecondary : Theme.textMuted
                        }
                        Text {
                            width: 30
                            horizontalAlignment: Text.AlignRight
                            anchors.verticalCenter: parent.verticalCenter
                            text: hrow.step === 0 ? "" : String(hrow.step)
                            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                            font.features: { "tnum": 1 }
                            color: Theme.textMuted
                        }
                    }
                }

                HistoryRow { step: 0; label: qsTr("Opened") }
                Repeater {
                    model: historyScroll.parent.history
                    HistoryRow {
                        required property var modelData
                        step: modelData.index
                        label: modelData.label
                    }
                }
            }
        }

        EmptyState {
            anchors.fill: parent
            visible: parent.history.length === 0
            iconName: "clock"
            title: qsTr("Nothing to undo")
            description: qsTr("Edits list here as you make them. Click one to go back to it.")
        }
    }
}
