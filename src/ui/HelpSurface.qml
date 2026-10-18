pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C

Item {
    id: root
    required property var shell
    anchors.fill: parent
    property var previousFocus: null
    property var topics: []
    property var selected: null
    property var guide: null
    readonly property bool tooltipVisible: tip.visible
    readonly property bool panelVisible: panel.visible

    function collectTopics() {
        const context = Help.current || (Help.hovered && Help.hovered.available ? Help.hovered : null)
                        || Help.forItem(previousFocus)
        topics = Array.from(Help.entries).filter(entry => entry.available && entry.purpose !== "")
        if (context && topics.indexOf(context) < 0) topics = [context].concat(topics)
        selected = context || (topics.length ? topics[0] : null)
        chooser.currentIndex = Math.max(0, topics.indexOf(selected))
        return context
    }
    function showHelp() {
        previousFocus = shell.activeFocusItem
        collectTopics()
        guide = null
        guideChooser.currentIndex = 0
        panel.open()
    }
    // The same panel, opened on a guide rather than a control.
    function showGuides(id) {
        previousFocus = shell.activeFocusItem
        collectTopics()
        guide = Help.guide(id) || Help.guides[0]
        guideChooser.currentIndex = Help.guides.indexOf(guide) + 1
        panel.open()
    }
    function guideText(g) {
        const lines = [g.title, g.summary]
        for (let i = 0; i < g.steps.length; ++i) lines.push((i + 1) + ". " + g.steps[i])
        return lines.join("\n\n")
    }
    Connections {
        target: root.shell
        function onActiveFocusItemChanged() {
            if (!panel.visible) {
                Help.dismiss()
                const item = root.shell.activeFocusItem
                // A click, popup closing or programmatic focus restoration is
                // not a request for help. Tab and Shift+Tab still explain it.
                if (item && (item.focusReason === Qt.TabFocusReason || item.focusReason === Qt.BacktabFocusReason))
                    Help.request(Help.forItem(item), false)
            }
        }
        function onActiveChanged() { if (!root.shell.active) Help.dismiss() }
    }
    Shortcut {
        sequence: root.shell.bindingFor("show_control_help")
        enabled: !panel.visible
        onActivated: root.showHelp()
    }
    // Cancel the delay as well as any visible card. Revealing another control
    // under a stationary pointer (scrolling or closing a popup) is not a hover.
    Binding {
        target: Help
        property: "pointerPressed"
        value: typeof keyWatcher !== "undefined" && keyWatcher.pointerPressed
    }
    Connections {
        target: typeof keyWatcher !== "undefined" ? keyWatcher : null
        function onInteractionStarted() { Help.interact() }
        function onPointerMoved() { Help.hoverAllowed = true }
    }
    // Retain only values, not the owner, for the exit transition. Clearing the
    // live request must not erase or move a card while it is still fading out.
    property var card: null
    readonly property var liveCard: {
        const control = Help.current
        const owner = control ? control.owner : null
        if (!owner) return null
        const geometry = owner.x + owner.y + owner.width + owner.height + root.width + root.height
        return { title: control.title, purpose: control.purpose, scope: control.scope,
                 reason: control.reason, shortcut: control.shortcut,
                 rect: owner.mapToItem(root, 0, 0, owner.width, owner.height) }
    }
    onLiveCardChanged: if (liveCard && liveCard.title) card = liveCard
    Tooltip {
        id: tip
        objectName: "controlTooltip"
        parent: root
        delay: 0 // Help owns the shared delay.
        // A control whose title has gone (its owner destroyed or its label
        // emptied) has nothing to say; the tooltip stays down rather than
        // showing an empty card.
        visible: Help.current !== null && !Help.blocked && !!Help.current.title
        text: root.card ? root.card.title : ""
        purpose: root.card ? root.card.purpose : ""
        scope: root.card ? root.card.scope : ""
        reason: root.card ? root.card.reason : ""
        shortcut: root.card ? root.card.shortcut : ""
        helpHint: root.shell.bindingFor("show_control_help")
                  ? qsTr("%1: more help").arg(root.shell.bindingFor("show_control_help")) : qsTr("More help in the Help menu")
        maximumWidth: Math.max(1, root.width - Theme.s2 * 2)
        readonly property rect ownerRect: root.card ? root.card.rect : Qt.rect(0, 0, 0, 0)
        x: Math.max(Theme.s2, Math.min(ownerRect.x, root.width - width - Theme.s2))
        y: {
            const below = ownerRect.y + ownerRect.height + Theme.s2
            const above = ownerRect.y - height - Theme.s2
            return Math.max(Theme.s2, Math.min(below + height <= root.height - Theme.s2 ? below : above,
                                              root.height - height - Theme.s2))
        }
    }
    C.Popup {
        id: panel
        objectName: "controlHelpPanel"
        parent: C.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(680, parent.width - Theme.s4 * 2)
        height: Math.min(560, parent.height - Theme.s4 * 2)
        padding: Theme.s4
        modal: true
        focus: true
        closePolicy: C.Popup.CloseOnEscape
        onAboutToShow: Help.blocked = true
        onOpened: (root.guide ? guideChooser : chooser).forceActiveFocus(Qt.TabFocusReason)
        onClosed: {
            if (root.previousFocus && root.previousFocus.visible && root.previousFocus.enabled)
                root.previousFocus.forceActiveFocus(Qt.OtherFocusReason)
            else root.shell.restoreFocus()
            Help.blocked = false
            Help.dismiss()
        }
        background: Rectangle {
            color: Theme.panelBg
            radius: Theme.rMenu
            border.color: Theme.borderStrong
            border.width: Theme.hairline
        }
        contentItem: Column {
            spacing: Theme.s3
            Text {
                text: qsTr("Help")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl * 1.5 * Help.textScale
                font.bold: true
            }
            Text {
                width: parent.width
                text: qsTr("Choose a guide for a task, or a control to learn what it does. Unavailable controls are included. Help works offline.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl * Help.textScale
            }
            ComboField {
                id: guideChooser
                objectName: "controlHelpGuide"
                caption: qsTr("Guide")
                width: parent.width
                model: [qsTr("None")].concat(Help.guides.map(g => g.title))
                helpText: qsTr("Short walkthroughs of everyday tasks: importing, recording, editing, effects, spectral repair, saving and exporting.")
                onActivated: index => root.guide = index > 0 ? Help.guides[index - 1] : null
            }
            ComboField {
                id: chooser
                objectName: "controlHelpChooser"
                caption: qsTr("Control")
                width: parent.width
                model: root.topics.map(entry => entry.title)
                helpText: qsTr("Every explained control in the current workspace, including ones that are unavailable right now.")
                onActivated: index => { root.selected = root.topics[index] || null; root.guide = null; guideChooser.currentIndex = 0 }
            }
            ScrollPane {
                id: scroll
                objectName: "controlHelpScroll"
                // Reaches into the card's margin, so the text keeps the column (ScrollPane).
                x: -reachLeft
                width: parent.width + reachLeft + reachRight
                height: Math.max(60, panel.availableHeight - y - footer.height - Theme.s3)
                clip: true
                contentWidth: availableWidth
                C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
                Text {
                    objectName: "controlHelpBody"
                    x: scroll.reachLeft; width: scroll.columnWidth
                    text: root.guide ? root.guideText(root.guide)
                        : root.selected ? [root.selected.title, root.selected.description, root.selected.topic]
                        .filter(part => part !== "").join("\n\n")
                        : qsTr("Hover over a control or focus it with Tab, then open this panel for more help.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl * Help.textScale
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
            Row {
                id: footer
                spacing: Theme.s2
                ToolButton {
                    objectName: "controlHelpTextSize"
                    text: Help.textScale > 1 ? qsTr("Standard text") : qsTr("Larger text")
                    onClicked: Help.textScale = Help.textScale > 1 ? 1 : 1.5
                }
                ToolButton {
                    objectName: "controlHelpClose"
                    text: qsTr("Close")
                    onClicked: panel.close()
                }
            }
        }
    }
}
