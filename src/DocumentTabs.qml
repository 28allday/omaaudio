pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The track tabs above the Waveform and Spectral editors: every track in the
// session; the checked one is focused. A session of forty tracks does not fit
// across the window, so the strip scrolls with the wheel, keeps the focused tab
// in view (Up and Down move the focus), caps each tab's width with the middle of
// a long name elided, and offers the whole list, searchable, from the button at
// its end.
Rectangle {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string tabObjectName: "docTab"
    // What a tab click acts on, for the tab's help.
    property string tabScope: ""
    // The page's own read-out at the right end (role and channels, analysis state).
    property string statusText: ""
    property bool statusVisible: true

    height: Theme.hDocTabs
    color: Theme.panelBg

    // Measured against the strip without the list button, which it shows: the
    // button narrowing the strip must not feed back into whether it is needed.
    readonly property bool overflowing:
        tabRow.implicitWidth > width - (status.visible ? status.implicitWidth + Theme.s2 : 0) - Theme.s2 * 2 + 0.5

    function revealFocused() {
        if (!service)
            return
        const tab = tabRepeater.itemAt(service.trackIndexById(service.focusedTrack))
        if (!tab)
            return
        if (tab.x < tabScroll.contentX)
            tabScroll.contentX = tab.x
        else if (tab.x + tab.width > tabScroll.contentX + tabScroll.width)
            tabScroll.contentX = Math.min(tab.x + tab.width - tabScroll.width,
                                          Math.max(0, tabScroll.contentWidth - tabScroll.width))
    }
    Connections {
        target: root.service
        function onTracksChanged() { Qt.callLater(root.revealFocused) }
    }
    onWidthChanged: Qt.callLater(revealFocused)
    Component.onCompleted: Qt.callLater(revealFocused)

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    Flickable {
        id: tabScroll
        objectName: root.objectName + "Scroll"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: listButton.visible ? listButton.left : status.left
        anchors.rightMargin: Theme.s2
        contentWidth: tabRow.implicitWidth
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        C.ScrollBar.horizontal: ScrollBar { showAtRest: false }

        // A plain wheel turn moves the strip sideways; a horizontal flick of a
        // touchpad does the same. Without this only a drag could reach the tabs
        // past the edge.
        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                const d = event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
                    ? (Math.abs(event.pixelDelta.x) > Math.abs(event.pixelDelta.y) ? event.pixelDelta.x : event.pixelDelta.y)
                    : (Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y) ? event.angleDelta.x : event.angleDelta.y) / 120 * 80
                tabScroll.contentX = Math.max(0, Math.min(tabScroll.contentX - d,
                                                          tabScroll.contentWidth - tabScroll.width))
                event.accepted = true
            }
        }

        Row {
            id: tabRow
            height: parent.height
            spacing: 0
            Repeater {
                id: tabRepeater
                model: root.service ? root.service.trackIdModel : null
                delegate: TabButton {
                    id: tabDelegate
                    required property var modelData
                    property int revision: 0   // this track's data changed (see MultitrackWorkspace)
                    Connections {
                        target: root.service
                        function onTrackDataChanged(ids) { if (ids.indexOf(tabDelegate.modelData) >= 0) ++tabDelegate.revision }
                    }
                    readonly property var tabTrack: { tabDelegate.revision; return root.service ? root.service.trackById(modelData) : null }
                    objectName: root.tabObjectName
                    text: tabTrack ? tabTrack.name : ""
                    Accessible.description: trackHelp.description
                    ControlHelp {
                        id: trackHelp
                        title: parent.text
                        purpose: qsTr("Focus this track and show its audio.")
                        scope: root.tabScope
                    }
                    uppercase: false
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, Theme.wDocTabMax)
                    height: root.height
                    checked: root.service !== null && root.service.focusedTrack === modelData
                    onClicked: if (root.service) root.service.focusTrack(modelData)
                }
            }
        }
    }

    // Every track, searchable: the way to a tab that is off the strip.
    IconButton {
        id: listButton
        objectName: root.objectName + "List"
        anchors.right: status.left
        anchors.rightMargin: status.visible && status.text !== "" ? Theme.s2 : 0
        anchors.verticalCenter: parent.verticalCenter
        visible: root.overflowing || trackList.opened
        iconName: "chevron-down"
        text: qsTr("All tracks")
        helpText: qsTr("List every track in the session and pick one to focus. Type to narrow the list.")
        helpScope: qsTr("Up and Down focus the next and previous track from anywhere in the editor.")
        checked: trackList.opened
        onClicked: trackList.opened ? trackList.close() : trackList.open()
    }

    C.Popup {
        id: trackList
        objectName: root.objectName + "Popup"
        x: Math.max(0, root.width - width - Theme.s2)
        y: root.height
        width: 320
        padding: Theme.s1
        focus: true
        closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutsideParent

        readonly property var shown: {
            if (!opened || !root.service)
                return []
            const all = root.service.trackSummaries
            const needle = filter.text.trim().toLowerCase()
            if (needle === "")
                return all
            var out = []
            for (var i = 0; i < all.length; ++i)
                if (String(all[i].name).toLowerCase().indexOf(needle) >= 0)
                    out.push(all[i])
            return out
        }
        function pick(id) {
            if (root.service) root.service.focusTrack(id)
            close()
        }

        onOpened: {
            filter.text = ""
            filter.forceActiveFocus()
            const i = root.service ? root.service.trackIndexById(root.service.focusedTrack) : -1
            if (i >= 0) rows.positionViewAtIndex(i, ListView.Center)
        }
        onClosed: if (root.shell) root.shell.restoreFocus()

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.rMenu
            border.width: Theme.hairline
            border.color: Theme.border
        }

        contentItem: Column {
            spacing: Theme.s1
            SearchField {
                id: filter
                objectName: root.objectName + "Search"
                width: parent.width
                placeholder: qsTr("Find a track…")
                // The list narrows as you type; only Return picks.
                live: false
                helpText: qsTr("Narrow the list to tracks whose name contains this text. Return focuses the first.")
                onAccepted: if (trackList.shown.length > 0) trackList.pick(trackList.shown[0].id)
            }
            ListView {
                id: rows
                width: parent.width
                height: Math.min(contentHeight, 12 * Theme.hRow)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                model: trackList.shown
                delegate: Rectangle {
                    id: listRow
                    objectName: root.objectName + "ListRow"
                    required property var modelData
                    readonly property bool current: root.service !== null && root.service.focusedTrack === modelData.id
                    width: ListView.view.width
                    height: Theme.hRow
                    radius: Theme.rControl
                    color: current ? (listHover.hovered ? Theme.selectedHover : Theme.selectedFill) : listHover.hovered ? Theme.hoverOverlay : "transparent"
                    HoverHandler { id: listHover }
                    TapHandler { onTapped: trackList.pick(listRow.modelData.id) }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s2
                        anchors.rightMargin: Theme.s2
                        verticalAlignment: Text.AlignVCenter
                        text: listRow.modelData.name
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.weight: listRow.current ? Theme.wHeading : Theme.wNormal
                        color: listRow.current ? Theme.accent : Theme.textPrimary
                    }
                }
            }
            Text {
                visible: trackList.shown.length === 0
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("No track name contains that.")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    // Last child on purpose: it is the strip's read-out, and stays clickable over
    // a long tab (tst_shell longTrackTabsDoNotStealClicksFromTheirStatus).
    Text {
        id: status
        objectName: "documentTabStatus"
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        visible: root.statusVisible
        text: root.statusText
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
}
