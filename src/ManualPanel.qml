pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import QtQuick.Layouts
import "ui"

// The user manual, as installed beside the application. Contents down the left,
// the chosen part on the right, a search that narrows the contents to the parts
// that mention a word. docs/user-manual.md is installed as manual.md and read
// from disk. The installation/technical README stays outside the help flow.
// Everything here is offline; nothing is fetched.
C.Popup {
    id: root
    objectName: "manualPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var manual: service ? service.manual() : ({})
    readonly property var allSections: manual.sections || []
    property string filter: ""
    property string referenceMessage: ""
    property int selected: 0

    readonly property var shown: {
        if (root.filter === "")
            return root.allSections
        const needle = root.filter.toLowerCase()
        const out = []
        for (const section of root.allSections)
            if (section.title.toLowerCase().indexOf(needle) >= 0
                || section.text.toLowerCase().indexOf(needle) >= 0)
                out.push(section)
        return out
    }
    readonly property var current: root.selected >= 0 && root.selected < root.shown.length
                                   ? root.shown[root.selected] : null

    function chooseSection(index, focusContents) {
        if (index < 0 || index >= shown.length) return
        selected = index
        const entry = contents.itemAt(index)
        if (focusContents && entry) entry.forceActiveFocus(Qt.TabFocusReason)
    }
    function followLink(link) {
        referenceMessage = ""
        if (link.startsWith("#")) {
            const anchor = decodeURIComponent(link.slice(1))
            for (let i = 0; i < allSections.length; ++i) {
                const slug = allSections[i].title.toLowerCase().replace(/[^\w\s-]/g, "").replace(/\s/g, "-")
                if (slug === anchor) {
                    search.text = ""
                    filter = ""
                    chooseSection(i, false)
                    return
                }
            }
        } else if (/^https?:\/\//.test(link)) Qt.openUrlExternally(link)
        else referenceMessage = qsTr("This supporting document is included in the source package, outside this manual: %1").arg(link)
    }
    onCurrentChanged: {
        referenceMessage = ""
        if (bodyScroll && bodyScroll.contentItem) bodyScroll.contentItem.contentY = 0
    }

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(940, parent ? parent.width - 32 : 940)
    height: Math.min(720, parent ? parent.height - 32 : 720)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    // Opened at a named part when something sends a person here for one thing
    // (Help › Keyboard Shortcuts), otherwise at the top.
    property string openAt: ""
    function showSection(title) { root.openAt = title || ""; root.open() }
    onOpened: {
        referenceMessage = ""
        search.text = ""
        root.filter = ""
        root.selected = 0
        if (root.openAt !== "") {
            for (let i = 0; i < root.allSections.length; ++i)
                if (root.allSections[i].title === root.openAt) { root.selected = i; break }
            root.openAt = ""
        }
        bodyScroll.contentItem.contentY = 0
        search.forceActiveFocus(Qt.TabFocusReason)
    }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    contentItem: ColumnLayout {
        spacing: Theme.s3

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Text {
                text: qsTr("User Manual"); color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl * 1.5 * Help.textScale
                font.bold: true
            }
            SearchField {
                id: search
                objectName: "manualSearch"
                Layout.fillWidth: true
                placeholder: qsTr("Search the manual…")
                helpTitle: qsTr("Search the manual")
                helpText: qsTr("Type a word to list only the parts of the manual that mention it.")
                helpScope: qsTr("Narrows the contents on the left. Nothing in the project changes.")
                onAccepted: function (text) { root.filter = text; root.selected = 0 }
                onCleared: { root.filter = ""; root.selected = 0 }
            }
            ToolButton {
                objectName: "manualTextSize"
                text: Help.textScale > 1 ? qsTr("Standard text") : qsTr("Larger text"); showLabel: true
                helpText: qsTr("Switch between standard and larger text in the manual and the help cards.")
                helpScope: qsTr("Display only, for this run.")
                onClicked: Help.textScale = Help.textScale > 1 ? 1 : 1.5
            }
            ToolButton {
                objectName: "manualClose"
                iconName: "x"; text: qsTr("Close")
                helpText: qsTr("Close the manual.")
                onClicked: root.close()
            }
        }

        Text {
            objectName: "manualMessage"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            visible: text !== ""
            color: root.manual.available ? Theme.textMuted : Theme.warning
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel * Help.textScale
            text: !root.manual.available ? (root.manual.error || qsTr("The manual is not installed."))
                : root.shown.length === 0 ? qsTr("Nothing in the manual mentions “%1”.").arg(root.filter)
                : root.referenceMessage
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: Theme.s3
            visible: root.manual.available

            // ── contents ────────────────────────────────────────────────
            Rectangle {
                Layout.preferredWidth: Math.min(260, root.width * 0.32)
                Layout.fillHeight: true
                color: Theme.panelRaised; radius: Theme.rControl
                border.width: Theme.hairline; border.color: Theme.border
                clip: true

                ScrollPane {
                    id: contentsScroll
                    anchors.fill: parent
                    anchors.margins: Theme.s1

                    Column {
                        x: contentsScroll.reachLeft; width: contentsScroll.columnWidth
                        Repeater {
                            id: contents
                            model: root.shown
                            delegate: C.AbstractButton {
                                id: entry
                                objectName: "manualEntry"
                                required property var modelData
                                required property int index
                                readonly property bool current: entry.index === root.selected
                                width: parent.width
                                height: label.implicitHeight + Theme.s2
                                hoverEnabled: true
                                focusPolicy: entry.current || entry.activeFocus ? Qt.TabFocus : Qt.NoFocus
                                Accessible.role: Accessible.ListItem
                                Accessible.name: entry.modelData.title
                                Accessible.selected: entry.current
                                background: Rectangle {
                                    radius: Theme.rControl
                                    color: entry.current ? (entry.hovered ? Theme.selectedHover : Theme.selectedFill)
                                         : entry.hovered ? Theme.hoverOverlay : "transparent"
                                    border.width: entry.visualFocus ? Theme.focusRing : 0
                                    border.color: Theme.accent
                                }
                                onClicked: root.chooseSection(entry.index, true)
                                onActiveFocusChanged: if (activeFocus) {
                                    root.selected = entry.index
                                    const view = contentsScroll.contentItem
                                    if (entry.y < view.contentY) view.contentY = entry.y
                                    else if (entry.y + entry.height > view.contentY + view.height)
                                        view.contentY = Math.min(entry.y + entry.height - view.height, Math.max(0, view.contentHeight - view.height))
                                }
                                Keys.onUpPressed: root.chooseSection(entry.index - 1, true)
                                Keys.onDownPressed: root.chooseSection(entry.index + 1, true)
                                Keys.onPressed: function (event) {
                                    if (event.key === Qt.Key_Home) { root.chooseSection(0, true); event.accepted = true }
                                    else if (event.key === Qt.Key_End) { root.chooseSection(root.shown.length - 1, true); event.accepted = true }
                                }
                                ControlHelp {
                                    title: entry.modelData.title
                                    purpose: qsTr("Read this part of the manual. Tab reaches the contents; Up and Down move between sections, Home and End reach the first and last.")
                                }
                                Text {
                                    id: label
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: Theme.s2 + (entry.modelData.level > 2 ? Theme.s3 : 0)
                                    anchors.rightMargin: Theme.s2
                                    text: entry.modelData.title
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    color: entry.current ? Theme.textPrimary : Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: (entry.modelData.level > 2 ? Theme.fsLabel : Theme.fsControl) * Help.textScale
                                    font.weight: entry.modelData.level > 2 ? Theme.wNormal : Theme.wHeading
                                }
                            }
                        }
                    }
                }
            }

            // ── the part being read ─────────────────────────────────────
            ScrollPane {
                id: bodyScroll
                objectName: "manualScroll"
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff

                Column {
                    x: bodyScroll.reachLeft; width: bodyScroll.columnWidth
                    spacing: Theme.s2
                    Text {
                        objectName: "manualTitle"
                        width: parent.width
                        text: root.current ? root.current.title : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl * 1.25 * Help.textScale
                        font.weight: Theme.wHeading
                    }
                    Text {
                        objectName: "manualBody"
                        width: parent.width
                        // Our own document, written here and installed with the
                        // application, so its markup is safe to render.
                        // MarkdownText ignores linkColor. Qt renders its own HTML
                        // with the current font and explicit anchor colours.
                        text: root.current && root.service ? root.service.manualHtml(root.current.text, font, linkColor) : ""
                        textFormat: Text.RichText
                        linkColor: Theme.accent
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl * Help.textScale
                        onLinkActivated: link => root.followLink(link)
                        Accessible.role: Accessible.StaticText
                        Accessible.name: root.current ? root.current.text : ""
                    }
                }
            }
        }

        Text {
            objectName: "manualPath"
            Layout.fillWidth: true
            elide: Text.ElideMiddle; textFormat: Text.PlainText
            visible: root.manual.available
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Installed at %1").arg(root.manual.path || "")
        }
    }
}
