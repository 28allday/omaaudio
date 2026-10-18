// Delegates below reach the browser's own `root` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The effect browser (brief section 11): search, and the installed effect catalogue.
// plugin. Click a row, or press Return, to add it to the focused track.
//
// What it has to get right besides that is honesty about what cannot be used --
// a plugin missing from the list looks like a broken install, so everything
// installed is shown and anything unusable says why, and says it rather than
// silently refusing the click.
//
// Modal by covering the window rather than by opening a second one.
Rectangle {
    id: root

    property var service: null
    property bool open: false
    property string filter: ""
    property bool managerMode: false
    property string collection: "all"
    property string category: ""
    property string author: ""
    // Whose chain a pick lands on: -1 the focused track, service.masterTarget the bus.
    property int target: -1
    // A pick for one clip's effects instead of a chain: clipTrack >= 0.
    property int clipTrack: -1
    property int clipIndex: -1
    property string clipStamp: ""

    anchors.fill: parent
    visible: open
    color: Theme.scrim

    // Which row Return would add. Reset on open and whenever the filter changes
    // the list under it, because keeping an index across a filter would arm the
    // wrong row.
    property int selected: 0

    signal added(string uri)
    signal clipEffectAdded(int track, int clip)
    // Choosing rather than adding: a recipe step (Main.pickEffect). The choice
    // goes out through `picked` and nothing is added to any chain.
    property bool picking: false
    signal picked(string uri)

    onOpenChanged: {
        if (!open) {
            root.picking = false
            root.clipTrack = -1; root.clipIndex = -1; root.clipStamp = ""
            root.filter = ""
            root.category = ""
            root.author = ""
            return
        }
        root.selected = 0
        if (root.service)
            root.service.listPlugins()
        root.forceActiveFocus()
    }

    onFilterChanged: root.selected = 0
    onShownChanged: root.selected = 0

    function activate(index) {
        if (!root.service || index < 0 || index >= root.shown.length)
            return
        const plugin = root.shown[index]
        if (root.picking) {
            if (!plugin.usable || plugin.disabled) return
            root.picked(plugin.uri)
            root.open = false
            return
        }
        if (root.clipTrack >= 0) {
            const track = root.clipTrack, clip = root.clipIndex
            if (root.service.addClipEffect(track, clip, plugin.uri, root.clipStamp)) {
                root.open = false
                root.clipEffectAdded(track, clip)
            }
            return
        }
        // Backend owns validation and error reporting, including disabled/missing
        // entries. Keep the browser open if the insertion was refused.
        if (root.service.addPlugin(plugin.uri, root.target)) {
            root.added(plugin.uri)
            root.open = false
        }
    }

    function reasonFor(plugin) {
        return plugin.disabled ? qsTr("Disabled for new effects") : plugin.reason
    }
    readonly property var catalogue: {
        if (!root.service) return []
        const favorite = root.service.favoritePluginUris
        const disabled = root.service.disabledPluginUris
        const recent = root.service.recentPluginUris
        const installed = root.service.plugins || []
        const out = []
        const seen = new Set()
        for (const item of installed) {
            out.push(Object.assign({}, item, { favorite: favorite.indexOf(item.uri) >= 0,
                                               disabled: disabled.indexOf(item.uri) >= 0 }))
            seen.add(item.uri)
        }
        for (const uri of favorite.concat(recent, disabled)) {
            if (seen.has(uri)) continue
            seen.add(uri)
            out.push({ uri: uri, name: uri, className: qsTr("Unavailable"), author: "", format: "",
                       usable: false, reason: root.service.pluginsListed ? qsTr("Not installed") : qsTr("Catalogue not loaded"),
                       favorite: favorite.indexOf(uri) >= 0, disabled: disabled.indexOf(uri) >= 0 })
        }
        return out
    }
    function choices(key) {
        const values = new Set()
        for (const plugin of root.catalogue)
            if (plugin[key]) values.add(plugin[key])
        return Array.from(values).sort()
    }
    readonly property var categories: [qsTr("All types")].concat(choices("className"))
    readonly property var authors: [qsTr("All makers")].concat(choices("author"))

    // Everything installed, newest filter applied. Rebuilt only when the
    // catalogue or the filter changes -- never per frame.
    readonly property var shown: {
        if (!root.service)
            return []
        const needle = root.filter.toLowerCase()
        var out = []
        const recent = root.service.recentPluginUris
        for (const p of root.catalogue) {
            if (root.collection === "favorites" && !p.favorite) continue
            if (root.collection === "recent" && recent.indexOf(p.uri) < 0) continue
            if (root.category && p.className !== root.category) continue
            if (root.author && p.author !== root.author) continue
            if (needle === "" || p.name.toLowerCase().indexOf(needle) >= 0
                || p.className.toLowerCase().indexOf(needle) >= 0
                || p.author.toLowerCase().indexOf(needle) >= 0
                || (p.format || "").toLowerCase() === needle
                || p.uri.toLowerCase().indexOf(needle) >= 0)
                out.push(p)
        }
        if (root.collection === "recent")
            out.sort((a, b) => recent.indexOf(a.uri) - recent.indexOf(b.uri))
        return out
    }

    readonly property int usableCount: {
        var n = 0
        for (var i = 0; i < root.shown.length; ++i)
            if (root.shown[i].usable && !root.shown[i].disabled)
                ++n
        return n
    }

    // Swallow anything aimed at the window behind it.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: root.open = false
    }

    Keys.onEscapePressed: root.open = false
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Backspace) {
            root.filter = root.filter.slice(0, -1)
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            root.selected = Math.min(root.selected + 1, root.shown.length - 1)
            list.positionViewAtIndex(root.selected, ListView.Contain)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            root.selected = Math.max(root.selected - 1, 0)
            list.positionViewAtIndex(root.selected, ListView.Contain)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (!root.managerMode) root.activate(root.selected)
            event.accepted = true
        } else if (event.text && event.text.length === 1 && event.text >= " ") {
            root.filter += event.text
            event.accepted = true
        }
    }

    // The dialog card.
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 860)
        height: Math.min(parent.height - Theme.s5 * 2, 620)
        color: Theme.panelBg
        radius: Theme.rMenu
        border.width: Theme.hairline
        border.color: Theme.border

        // Clicks on the card stay on the card.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: {} }

        Column {
            anchors.fill: parent
            anchors.margins: Theme.s4
            spacing: Theme.s3

            Row {
                width: parent.width
                spacing: Theme.s3

                Icon { name: "sparkles"; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: root.managerMode ? qsTr("Plug-in Manager") : root.picking ? qsTr("Choose an Effect for the Recipe") : qsTr("Add Effect")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsTitle
                    font.weight: Theme.wHeading
                    color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                    text: {
                        if (!root.service)
                            return ""
                        if (root.service.scanning)
                            return qsTr("scanning…")
                        if (!root.service.pluginsListed)
                            return ""
                        return qsTr("%1 shown, %2 usable as inserts").arg(root.shown.length).arg(root.usableCount)
                    }
                }
                Item { width: 1; height: 1 }
            }

            Row {
                width: parent.width; spacing: Theme.s2
                ComboField {
                    objectName: "pluginCollection"; width: 145
                    helpTitle: qsTr("Collection")
                    helpText: qsTr("Show every installed effect, only the ones you starred, or the effects you added most recently.")
                    model: [qsTr("All effects"), qsTr("Starred"), qsTr("Recent")]
                    currentIndex: ["all", "favorites", "recent"].indexOf(root.collection)
                    onActivated: { root.collection = ["all", "favorites", "recent"][currentIndex]; root.forceActiveFocus() }
                }
                ComboField {
                    objectName: "pluginCategory"; width: Math.max(150, (parent.width - 145 - Theme.s2 * 2) / 2)
                    helpTitle: qsTr("Type")
                    helpText: qsTr("Show only effects of one class, as their makers describe them: compressors, reverbs and so on.")
                    model: root.categories
                    currentIndex: root.category ? root.categories.indexOf(root.category) : 0
                    onActivated: { root.category = currentIndex > 0 ? root.categories[currentIndex] : ""; root.forceActiveFocus() }
                }
                ComboField {
                    objectName: "pluginAuthor"; width: Math.max(150, (parent.width - 145 - Theme.s2 * 2) / 2)
                    helpTitle: qsTr("Maker")
                    helpText: qsTr("Show only effects from one maker.")
                    model: root.authors
                    currentIndex: root.author ? root.authors.indexOf(root.author) : 0
                    onActivated: { root.author = currentIndex > 0 ? root.authors[currentIndex] : ""; root.forceActiveFocus() }
                }
            }

            // The filter is typed straight at the browser rather than into a text
            // field: the shell already owns the keymap, and a focused TextInput
            // inside it would have to fight for every keystroke. Drawn as the
            // search field it behaves like.
            Rectangle {
                width: parent.width
                height: Theme.hControl + Theme.s1
                color: Theme.controlBg
                radius: Theme.rControl
                border.width: Theme.hairline
                border.color: Theme.accent

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s2
                    spacing: Theme.s2
                    Icon { name: "search"; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.filter === "" ? qsTr("Type to search by name, class or author") : root.filter
                        textFormat: Text.PlainText
                        color: root.filter === "" ? Theme.textMuted : Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                    }
                }
            }

            C.ScrollView {
                id: scanWarnings
                objectName: "pluginScanWarnings"
                width: parent.width
                visible: !!root.service && (root.service.pluginScanWarnings || []).length > 0
                height: visible ? Math.min(80, warningText.implicitHeight + Theme.s2) : 0
                clip: true
                contentWidth: availableWidth
                Text {
                    id: warningText
                    // Room for the scrollbar beside it.
                    width: scanWarnings.availableWidth - Theme.s3
                    text: root.service ? (root.service.pluginScanWarnings || []).join("\n") : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                }
            }

            // Column header.
            Row {
                width: parent.width
                Text { width: 32; text: qsTr("Fav"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: (parent.width - 80) * 0.43; text: qsTr("Name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: (parent.width - 80) * 0.25; text: qsTr("Class"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: (parent.width - 80) * 0.32; text: qsTr("Availability / ports"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { width: 40; visible: root.managerMode; text: qsTr("Use"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            }
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            ListView {
                id: list
                objectName: "pluginList"
                width: parent.width
                height: parent.height - y - footer.height - Theme.s3
                clip: true
                model: root.shown
                // 360 plugins with a thousand ports between some of them: the list
                // is virtualised by ListView and the delegate stays cheap on purpose.
                cacheBuffer: 200
                C.ScrollBar.vertical: ScrollBar { id: listBar }

                Text {
                    anchors.centerIn: parent; width: parent.width - Theme.s4
                    visible: root.shown.length === 0; horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    text: root.service && root.service.scanning ? qsTr("Reading installed effects…")
                        : root.collection === "favorites" && root.filter === "" ? qsTr("No starred effects match. Star one in All effects.")
                        : root.collection === "recent" && root.filter === "" ? qsTr("No recent effects match. Effects appear here after you add an effect.")
                        : qsTr("No effects match these filters.")
                }

                delegate: Item {
                    id: entry
                    objectName: "pluginEntry"
                    required property var modelData
                    required property int index
                    width: list.width - listBar.gutter
                    height: Theme.hRow + Theme.s2

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.rControl
                        visible: entry.index === root.selected
                        color: Theme.controlBg
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: root.managerMode || (entry.modelData.usable && !entry.modelData.disabled) ? Qt.PointingHandCursor
                                                            : Qt.ForbiddenCursor
                        onEntered: root.selected = entry.index
                        onClicked: {
                            root.selected = entry.index
                            if (!root.managerMode) root.activate(entry.index)
                        }
                        ControlHelp {
                            title: entry.modelData.name
                            purpose: root.managerMode ? qsTr("An installed effect. The star keeps it in Starred; the switch allows or refuses it for new effects.")
                                                      : root.picking ? qsTr("An installed effect. Click, or press Return with it selected, to add it to the recipe as a step.")
                                                      : qsTr("An installed effect. Click, or press Return with it selected, to add it to the chain.")
                            scope: (entry.modelData.className || "") + (entry.modelData.author ? " · " + entry.modelData.author : "") + (entry.modelData.format ? " · " + entry.modelData.format : "")
                            unavailableReason: !entry.modelData.usable || entry.modelData.disabled ? root.reasonFor(entry.modelData) : ""
                        }
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s1
                        anchors.right: parent.right
                        spacing: 0

                        ToolButton {
                            objectName: "pluginFavorite"
                            width: 32; height: Theme.hRow
                            iconName: "star"; text: entry.modelData.favorite ? qsTr("Unstar") : qsTr("Star")
                            helpText: entry.modelData.favorite ? qsTr("Take this effect out of Starred.")
                                                               : qsTr("Keep this effect in Starred for quick finding.")
                            helpScope: qsTr("A private preference on this computer. Nothing in the project changes.")
                            checked: entry.modelData.favorite
                            onClicked: root.service.setPluginFavorite(entry.modelData.uri, !entry.modelData.favorite)
                        }
                        Text {
                            width: (entry.width - 124) * 0.43
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            color: !entry.modelData.usable || entry.modelData.disabled ? Theme.textMuted
                                 : entry.index === root.selected ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsControl
                            text: entry.modelData.name
                        }
                        // The plugin format: LV2 and CLAP sit in the same list, and
                        // a project file names which one an insert is.
                        Text {
                            objectName: "pluginFormat"
                            width: 44
                            textFormat: Text.PlainText
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            text: entry.modelData.format || ""
                        }

                        Text {
                            width: (entry.width - 124) * 0.25
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            text: entry.modelData.className
                        }

                        Text {
                            width: (entry.width - 124) * 0.32
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            color: entry.modelData.usable && !entry.modelData.disabled ? Theme.textMuted : Theme.warning
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            text: {
                                if (!entry.modelData.usable || entry.modelData.disabled)
                                    return root.reasonFor(entry.modelData)
                                var shape = entry.modelData.layout === "either"
                                          ? qsTr("mono or stereo") : entry.modelData.layout
                                if (entry.modelData.sidechain > 0)
                                    shape += qsTr(", sidechain")
                                return shape + ", " + (entry.modelData.controls === 1
                                       ? qsTr("1 control") : qsTr("%1 controls").arg(entry.modelData.controls))
                            }
                        }
                        Toggle {
                            objectName: "pluginEnabled"; width: 40
                            visible: root.managerMode; text: qsTr("Allow new effects")
                            helpText: qsTr("When off, this effect cannot be added to a chain. Effects already in a project keep working.")
                            helpScope: qsTr("A private preference on this computer; it does not uninstall the plugin.")
                            checked: !entry.modelData.disabled
                            onClicked: root.service.setPluginEnabled(entry.modelData.uri, entry.modelData.disabled)
                        }
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: Theme.hairline
                        color: Theme.border
                        opacity: 0.6
                    }
                }
            }

            Column {
                id: footer
                width: parent.width
                spacing: Theme.s2
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: root.managerMode ? qsTr("Disabling affects new effects only. Existing project effects keep working. Newly installed plugins appear after restarting OmaAudio.")
                        : root.picking ? qsTr("Return adds the selected effect to the recipe  ·  ↑↓ choose  ·  Escape closes")
                        : qsTr("Return adds the selected effect  ·  ↑↓ choose  ·  Escape closes")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                }
                Text {
                    objectName: "pluginLibraryError"; width: parent.width; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    visible: text !== ""; text: root.service ? root.service.lastError : ""
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.warning
                }
                Row {
                    spacing: Theme.s2
                    ToolButton {
                        text: qsTr("Read installed effects"); showLabel: true
                        helpText: qsTr("Look again for the plugins installed on this computer, LV2 and CLAP.")
                        helpScope: qsTr("Plugins installed while OmaAudio is running appear after a restart.")
                        visible: root.service && !root.service.pluginsListed && !root.service.scanning
                        onClicked: root.service.listPlugins()
                    }
                    ToolButton {
                        objectName: "pluginClearRecent"; text: qsTr("Clear recent list"); showLabel: true
                        helpText: qsTr("Forget which effects were added recently. Nothing in the project changes.")
                        visible: root.collection === "recent"
                        onClicked: root.service.clearRecentPlugins()
                    }
                    ToolButton { objectName: "pluginClose"; text: qsTr("Close"); showLabel: true; helpText: qsTr("Close the browser without adding an effect."); onClicked: root.open = false }
                }
            }
        }
    }
}
