import QtQuick
import QtQuick.Window
import QtQml.Models
import QtQuick.Controls.Basic as C
import "ui"

// The 44 px application bar: identity, the menu bar inline, workspace
// navigation centred, document actions and window controls at the right. On a
// frameless Linux window this strip is the drag handle and the double-click
// maximise target, so the empty space between groups is deliberately live.
Rectangle {
    id: root

    property Window target: null
    // The shell (Main.qml): actions, keymap, dialogs, layout state.
    property var shell: null
    readonly property var service: shell ? shell.service : null

    // The Effects menu (Backend.effectMenu, see effectmenu.h): the core set by
    // group, then All Effects with every installed effect, one entry per family.
    // Built imperatively: a group mixes plain items with variant submenus, in
    // order, which two Instantiators cannot keep.
    readonly property var effectMenuModel: root.service ? root.service.effectMenu : ({})
    readonly property var effectGroupLabels: ({
        dynamics: qsTr("Dynamics"), eq: qsTr("EQ and Filter"), reverb: qsTr("Reverb"),
        delay: qsTr("Delay"), modulation: qsTr("Modulation"), distortion: qsTr("Distortion"),
        space: qsTr("Stereo and Space"), restoration: qsTr("Restoration"), pitch: qsTr("Pitch"),
        meters: qsTr("Meters and Analysis"), utility: qsTr("Utility"), other: qsTr("Other")
    })
    readonly property var effectVariantLabels: ({
        stereo: qsTr("Stereo"), either: qsTr("Stereo"), mono: qsTr("Dual Mono"),
        leftright: qsTr("Left/Right"), midside: qsTr("Mid/Side")
    })
    property var effectMenuObjects: []     // everything made, to destroy on rebuild
    property var effectMenuTop: []         // menus put straight in the Effects menu
    onEffectMenuModelChanged: rebuildEffectsMenu()
    Component.onCompleted: { rebuildEffectsMenu(); rebuildRecipesMenu() }

    function rebuildEffectsMenu() {
        if (!effectsMenu)
            return
        for (const menu of root.effectMenuTop)
            effectsMenu.removeMenu(menu)
        for (const object of root.effectMenuObjects)
            object.destroy()
        const made = [], top = []
        const core = root.effectMenuModel.core || []
        const all = root.effectMenuModel.all || []
        // Straight after the loading line, which holds their place while the
        // catalogue is read.
        let at = 0
        for (let i = 0; i < effectsMenu.count; ++i)
            if (effectsMenu.itemAt(i) && effectsMenu.itemAt(i).objectName === "effectsMenuLoading") at = i + 1
        for (const group of core) {
            const menu = groupMenu(group, "effectGroup_", made)
            top.push(menu)
            effectsMenu.insertMenu(at++, menu)
        }
        if (all.length > 0) {
            const allMenu = effectGroupMenu.createObject(effectMenuHost, { objectName: "allEffectsMenu", title: qsTr("All Effects") })
            made.push(allMenu)
            for (const group of all)
                allMenu.addMenu(groupMenu(group, "allEffectGroup_", made))
            top.push(allMenu)
            effectsMenu.insertMenu(at++, allMenu)
        }
        root.effectMenuObjects = made
        root.effectMenuTop = top
    }
    // One group's submenu: its entries, then a Sidechain submenu when it has any.
    function groupMenu(group, prefix, made) {
        const menu = effectGroupMenu.createObject(effectMenuHost, { objectName: prefix + group.key, title: root.effectGroupLabels[group.key] || group.key })
        made.push(menu)
        addEffectEntries(menu, group.entries, made)
        if (group.sidechain.length > 0) {
            const side = effectGroupMenu.createObject(effectMenuHost, { objectName: "effectSidechain_" + group.key, title: qsTr("Sidechain") })
            made.push(side)
            addEffectEntries(side, group.sidechain, made)
            menu.addMenu(side)
        }
        return menu
    }
    // A family is one item; with Left/Right or Mid/Side it is a submenu of its versions.
    function addEffectEntries(menu, entries, made) {
        for (const entry of entries) {
            if (entry.variants.length === 0) {
                const item = effectEntryItem.createObject(effectMenuHost, { entry: entry, uri: entry.uri, text: Theme.shortName(entry.name, 48), helpTitle: entry.name })
                made.push(item)
                menu.addItem(item)
                continue
            }
            const versions = effectGroupMenu.createObject(effectMenuHost, { objectName: "effectVariants", title: entry.name })
            made.push(versions)
            for (const version of entry.variants) {
                const label = root.effectVariantLabels[version.variant] || version.variant
                const item = effectEntryItem.createObject(effectMenuHost, { entry: entry, uri: version.uri, text: label,
                                                                   versionName: entry.name + " (" + label + ")" })
                made.push(item)
                versions.addItem(item)
            }
            menu.addMenu(versions)
        }
    }
    // Effects › Recipes: built in, then your own after a line, inserted above
    // Run Last Recipe Again. Built imperatively for the same reason as the
    // effect groups: repeated entries in a menu do not keep their order when
    // the list changes.
    readonly property var recipeList: root.service ? root.service.recipes : []
    property var recipeMenuObjects: []
    onRecipeListChanged: rebuildRecipesMenu()
    function rebuildRecipesMenu() {
        if (!recipesMenu)
            return
        for (const object of root.recipeMenuObjects)
            recipesMenu.removeItem(object)
        const made = []
        let at = 0
        const own = root.recipeList.filter(r => r.builtIn !== true)
        for (const recipe of root.recipeList.filter(r => r.builtIn === true)) {
            const item = recipeEntryItem.createObject(effectMenuHost, { recipeId: recipe.id, text: Theme.shortName(recipe.name, 40), helpTitle: recipe.name, own: false })
            made.push(item)
            recipesMenu.insertItem(at++, item)
        }
        if (own.length > 0) {
            const line = recipeSeparator.createObject(effectMenuHost)
            made.push(line)
            recipesMenu.insertItem(at++, line)
        }
        for (const recipe of own) {
            const item = recipeEntryItem.createObject(effectMenuHost, { recipeId: recipe.id, text: Theme.shortName(recipe.name, 40), helpTitle: recipe.name, own: true })
            made.push(item)
            recipesMenu.insertItem(at++, item)
        }
        root.recipeMenuObjects = made
    }
    Component {
        id: recipeEntryItem
        AppMenuItem {
            property string recipeId: ""
            property bool own: false
            objectName: "recipeMenu_" + recipeId
            enabled: root.service !== null && !root.service.recipeRunning
            helpText: own ? qsTr("Run this recipe of your own on the selection, or on the whole active track when nothing is selected.")
                          : qsTr("Run this recipe on the selection, or on the whole active track when nothing is selected.")
            helpScope: qsTr("Written into the audio as one Undo step. The Recipes tab shows each step as it runs.")
            reason: qsTr("Another recipe is running.")
            onTriggered: if (root.shell) root.shell.runRecipe(recipeId)
        }
    }
    Component {
        id: recipeSeparator
        MenuSep {}
    }

    // Owns what the builder makes, so it is found under the bar like any child.
    // An Item: menu items are graphical and want a place in the scene until a
    // menu takes them.
    Item { id: effectMenuHost; visible: false }
    Component {
        id: effectGroupMenu
        AppMenu { shell: root.shell }
    }
    // Picking an effect does what the page is for: on the Waveform page with a
    // time selection it is applied to the selection, otherwise it goes on the
    // active track (see Main.qml chooseEffect).
    Component {
        id: effectEntryItem
        AppMenuItem {
            property var entry: ({})
            property string uri: ""
            property string versionName: ""
            objectName: "effectItem"
            shell: root.shell
            helpText: root.shell ? root.shell.effectChoiceHelp(entry, versionName || entry.name) : ""
            onTriggered: if (root.shell) root.shell.chooseEffect(uri, entry)
        }
    }
    property alias workspace: nav.current

    signal workspaceActivated(string name)

    implicitHeight: Theme.hTitleBar
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    TapHandler {
        onDoubleTapped: if (root.target)
            root.target.visibility = root.target.visibility === Window.Maximized
                                     ? Window.Windowed : Window.Maximized
    }
    DragHandler {
        target: null
        onActiveChanged: if (active && root.target) root.target.startSystemMove()
    }

    // ── identity ────────────────────────────────────────────────────────
    Row {
        id: identity
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "audio-waveform"
            size: 20
            color: Theme.accent
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.appName
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsTitle
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
    }

    // ── menus ───────────────────────────────────────────────────────────
    C.MenuBar {
        id: menuBar
        objectName: "menuBar"
        anchors.left: identity.right
        anchors.leftMargin: Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        background: Item {}
        delegate: C.MenuBarItem {
            id: mbi
            implicitHeight: Theme.hControl + Theme.s1
            padding: Theme.s1 + 2
            contentItem: Text {
                text: mbi.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
                color: mbi.highlighted ? Theme.textPrimary : Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.rControl
                color: mbi.highlighted ? Theme.hoverOverlay : "transparent"
            }
        }

        AppMenu {
            objectName: "menu_File"; title: qsTr("File"); shell: root.shell
            AppMenuItem {
                text: qsTr("New Project"); actionName: "new_project"
                helpText: qsTr("Start an empty project.")
                helpScope: qsTr("Replaces the current project. You are asked to save unsaved changes first.")
            }
            AppMenuItem {
                text: qsTr("Record…"); actionName: "record"
                helpText: qsTr("Open the Recording panel to choose inputs and start a take.")
            }
            AppMenuItem {
                text: qsTr("Open Audio…"); actionName: "open_file"
                helpText: qsTr("Open an audio file as a new project.")
                helpScope: qsTr("Replaces the current project. You are asked to save unsaved changes first.")
            }
            AppMenuItem {
                text: qsTr("Open Project…"); actionName: "open_project"
                helpText: qsTr("Open a saved OmaAudio project bundle.")
                helpScope: qsTr("Replaces the current project. You are asked to save unsaved changes first.")
            }
            AppMenuItem {
                objectName: "openRecentDocuments"; text: qsTr("Recent Documents…"); actionName: "show_recent_documents"
                helpText: qsTr("List recently saved projects and opened files, to reopen one or import its audio as a track.")
            }
            AppMenuItem {
                text: qsTr("Import Audio as Track…"); actionName: "import_file"
                helpText: qsTr("Add an audio file to the current project as a new track.")
                helpScope: qsTr("The project is kept. Audio at another sample rate is converted to the project rate.")
            }
            AppMenuItem {
                objectName: "menuImportSession"; text: qsTr("Import Session (AAF/OMF)…"); actionName: "import_session"
                helpText: qsTr("Import audio tracks, clip positions and source offsets from an AAF or OMF session, such as Resolve’s Pro Tools export or an OMF from a picture editor.")
                helpScope: qsTr("Adds tracks to this project in one Undo step. The import panel helps locate media and reports unsupported effects.")
            }
            AppMenuItem {
                objectName: "menuImportVideo"
                text: qsTr("Import Video Reference…"); onTriggered: root.shell.runAction("import_video")
                helpText: qsTr("Choose a video to cut the sound against. Its picture follows the playhead in the Video tab of the right dock; no sound is taken from it.")
                helpScope: qsTr("One picture per project, saved with it. Needs FFmpeg.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Save"); actionName: "save"
                helpText: qsTr("Write the project to its project bundle.")
                helpScope: qsTr("The first save asks where to put the project.")
            }
            AppMenuItem {
                text: qsTr("Save As…"); actionName: "save_as"
                helpText: qsTr("Write the project to a new project bundle and continue working in it.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Autosave"); actionName: "toggle_autosave"; checkable: true
                checked: root.shell ? root.shell.autosaveSeconds > 0 : true
                helpText: qsTr("Write a recovery copy of the project automatically a short while after each change.")
                helpScope: qsTr("The interval is set in Preferences. An interrupted project is offered for recovery at the next launch.")
            }
            AppMenuItem {
                text: qsTr("Autosave Now"); actionName: "autosave_now"; enabled: root.service !== null && root.service.dirty
                helpText: qsTr("Write the recovery copy immediately.")
                helpScope: qsTr("Does not replace saving the project.")
                reason: qsTr("There are no unsaved changes to write.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Export Audio…"); actionName: "export"; enabled: root.service !== null && root.service.trackCount > 0
                helpText: qsTr("Export the finished audio using the settings on the Mastering page.")
                helpScope: qsTr("Includes the mix and its effects. You can keep working while the file is exported.")
                reason: qsTr("Open or import audio first.")
            }
            AppMenuItem {
                text: qsTr("Collect Project…"); actionName: "collect_project"
                helpText: qsTr("Copy the project, source audio and reference video into a new portable project folder.")
                helpScope: qsTr("Includes a required-plugin list. Existing destinations and the open session are unchanged.")
            }
            AppMenuItem {
                text: qsTr("Process Many Files…"); actionName: "show_file_batch"
                helpText: qsTr("Process independent audio files with shared effect and output settings, with a separate result for each file.")
                helpScope: qsTr("Does not change the project, originals or existing output files.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "menuMetadata"
                text: qsTr("Metadata…"); onTriggered: root.shell.runAction("show_metadata")
                helpText: qsTr("The project's broadcast description, the marker export choice, and a preview of what the next export writes.")
            }
            AppMenuItem {
                text: qsTr("Storage and Cleanup…"); actionName: "show_storage"
                helpText: qsTr("See what audio the project holds in memory and on disk, and release what nothing uses.")
            }
            AppMenuItem { text: qsTr("Quit"); shortcut: "Ctrl+Q"; onTriggered: if (root.target) root.target.close(); helpText: qsTr("Close OmaAudio. A project with unsaved changes asks first.") }
        }
        AppMenu {
            objectName: "menu_Edit"; title: qsTr("Edit"); shell: root.shell
            AppMenuItem { text: qsTr("Undo"); actionName: "undo"; enabled: root.service !== null && root.service.canUndo; reason: qsTr("Nothing to undo."); helpText: qsTr("Take back the last edit. The status bar names it."); helpScope: qsTr("Every edit is an Undo step; the History tab lists them all.") }
            AppMenuItem { text: qsTr("Redo"); actionName: "redo"; enabled: root.service !== null && root.service.canRedo; reason: qsTr("Nothing to redo."); helpText: qsTr("Put back the edit that Undo took away.") }
            MenuSep {}
            AppMenuItem { text: qsTr("Cut"); actionName: "cut"; helpText: qsTr("Copy and remove the selected audio. Whole clips leave gaps; a time range closes its gap when Ripple Edits is on."); helpScope: qsTr("Uses the selected whole clips or the time range across selected tracks. Ctrl-click clips or track headers to choose several.") }
            AppMenuItem { text: qsTr("Copy"); actionName: "copy"; helpText: qsTr("Copy the selected audio to the clipboard, leaving the track as it is."); helpScope: qsTr("Uses the selected whole clips or the time range across selected tracks. Ctrl-click clips or track headers to choose several.") }
            AppMenuItem { text: qsTr("Paste"); actionName: "paste"; helpText: qsTr("Paste at the playhead, starting on the focused track. Multiple copied tracks keep their spacing; Ripple Edits opens time on each destination."); helpScope: qsTr("Whole-clip paste preserves audio between copied clips; range paste replaces the full interval. One Undo step.") }
            AppMenuItem {
                text: qsTr("Paste to New Track"); actionName: "paste_to_new"
                helpText: qsTr("Paste at the start of new tracks, keeping copied track spacing and the complete time range. One Undo step.")
            }
            AppMenuItem {
                text: qsTr("Delete"); actionName: "delete_selection"
                helpText: qsTr("Remove selected whole clips, leaving gaps, or the selected time range across tracks. Ripple closes time-range gaps only.")
            }
            AppMenuItem {
                text: qsTr("Delete, Close the Gap"); actionName: "delete_close_gap"
                helpText: qsTr("Remove an explicit time range from the selected tracks and move later audio earlier to fill the gap. Select a time range before using this with whole clips.")
            }
            AppMenuItem {
                text: qsTr("Delete Time Across All Tracks"); actionName: "delete_time_all_tracks"
                helpText: qsTr("Remove the selected time from every track and close the gap on each.")
                helpScope: qsTr("Later audio on every track moves earlier. Undo restores it.")
                enabled: root.service !== null && !root.service.selectionEmpty && root.service.selectedClips.length === 0
                reason: qsTr("Select a time range first.")
            }
            AppMenuItem {
                text: qsTr("Silence"); actionName: "silence"
                helpText: qsTr("Replace the selected audio with silence. Timing is unchanged.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "editTidyPauses"
                text: qsTr("Tidy Pauses…"); actionName: "show_tidy_pauses"
                helpText: qsTr("Find the gaps between words, then shorten, remove or mark them. Opens the page in the Sound tab, looking in the selection if there is one.")
                helpScope: qsTr("Nothing changes until you choose. One Undo step.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Snapping"); actionName: "toggle_snap"; checkable: true
                checked: root.shell ? root.shell.snapping : true
                helpText: root.service ? root.service.snapHint : ""
            }
            AppMenu {
                objectName: "menu_SnapGrid"; title: qsTr("Snap Grid"); shell: root.shell
                Repeater {
                    model: root.service ? root.service.snapGrids : []
                    AppMenuItem {
                        required property string modelData
                        objectName: "snapGrid_" + modelData
                        text: root.service ? root.service.snapGridLabel(modelData) : modelData
                        checkable: true
                        checked: root.shell ? root.shell.snapGrid === modelData : modelData === "none"
                        helpText: qsTr("Choose a grid for selection edges and moved clips. Beat divisions follow the project's time signature.")
                        helpScope: qsTr("Requires Snapping and musical timing in Project properties. Nearby clip edges, markers and the playhead take priority. This editing preference does not move existing audio.")
                        onTriggered: if (root.shell) root.shell.snapGrid = modelData
                    }
                }
            }
            AppMenuItem {
                text: qsTr("Ripple Edits"); actionName: "toggle_ripple"; checkable: true
                checked: root.shell ? root.shell.ripple : false
                helpText: qsTr("Time-range Delete and Cut close the gap; Paste pushes later audio along. Whole-clip deletion keeps other clips in place.")
                helpScope: qsTr("Later audio on the edited track changes position. Check timing after an edit.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Loop Selected Range"); actionName: "toggle_loop"; checkable: true
                checked: root.service !== null && root.service.loopEnabled
                helpText: qsTr("Play the selected time range round and round.")
                enabled: root.service !== null && (root.service.loopEnabled || !root.service.selectionEmpty)
                reason: qsTr("Select a time range first.")
            }
            AppMenuItem {
                objectName: "toggleMetronome"; text: qsTr("Metronome"); actionName: "toggle_metronome"; checkable: true
                checked: root.service !== null && root.service.metronome
                helpText: qsTr("Click on each beat while playing or recording, with a higher click on the first beat of each bar.")
                helpScope: qsTr("Follows the tempo and meter in Project Properties. Heard only: never recorded, measured or exported.")
            }
            AppMenuItem { text: qsTr("Select All"); actionName: "select_all"; helpText: qsTr("Select the whole length of the project.") }
            AppMenuItem { text: qsTr("Select None"); actionName: "clear_selection"; helpText: qsTr("Drop the selection. The playhead stays where it is.") }
            AppMenuItem {
                text: qsTr("Selection Start at Playhead"); actionName: "select_start"
                helpText: qsTr("Move the start of the selection to the playhead.")
            }
            AppMenuItem {
                text: qsTr("Selection End at Playhead"); actionName: "select_end"
                helpText: qsTr("Move the end of the selection to the playhead.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "openPreferences"; text: qsTr("Preferences…"); actionName: "show_preferences"
                helpText: qsTr("Settings that are yours rather than the project's: autosave, editing, the spectral display and delivery defaults.")
            }
        }
        AppMenu {
            objectName: "menu_Clip"; title: qsTr("Clip"); shell: root.shell
            AppMenuItem {
                text: qsTr("Move Tool"); actionName: "timeline_move"; checkable: true
                checked: root.shell ? root.shell.timelineMoveTool : false
                helpText: qsTr("Drag a selected clip to move the whole selection to an empty position or other tracks. Ctrl-click toggles clips.")
                helpScope: qsTr("Keeps clip spacing and sources. Red previews refuse the whole move; Escape cancels.")
            }
            AppMenuItem {
                text: qsTr("Selection Tool"); actionName: "timeline_select"
                helpText: qsTr("Drag across a lane to select a time range.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "openClipGroups"; text: qsTr("Clip Groups…"); actionName: "show_clip_groups"
                helpText: qsTr("Keep related clips linked across sessions. Create, name, colour, detach or dissolve groups.")
                helpScope: qsTr("Uses OmaAudio's existing whole-clip selection and move controls. Copies form independent groups.")
            }
            AppMenuItem {
                objectName: "linkClipGroups"; text: qsTr("Link Group Selection"); actionName: "toggle_clip_group_link"; checkable: true
                checked: root.service ? root.service.clipGroupsLinked : true
                helpText: qsTr("Select group members together for moving, copying, cutting and deleting whole clips.")
                helpScope: qsTr("Turn off temporarily to select members independently. New and Open restore linking. Time ranges and individual properties keep their own scope.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Split at Playhead"); actionName: "split"
                helpText: qsTr("Divide the clip at the playhead so you can move or edit each part.")
                helpScope: qsTr("Acts on the selected track. No audio is removed; Undo joins the parts again.")
            }
            AppMenuItem {
                text: qsTr("Trim to Selection"); actionName: "trim"
                helpText: qsTr("Keep only the selected part of the track's audio.")
            }
            MenuSep {}
            AppMenuItem { text: qsTr("Fade In"); actionName: "fade_in"; helpText: qsTr("Fade the selected audio up from silence at its start."); helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.") }
            AppMenuItem { text: qsTr("Fade Out"); actionName: "fade_out"; helpText: qsTr("Fade the selected audio down to silence at its end."); helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.") }
            AppMenuItem { text: qsTr("Gain +1 dB"); actionName: "gain_up"; helpText: qsTr("Make the selected audio 1 dB louder."); helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.") }
            AppMenuItem { text: qsTr("Gain −1 dB"); actionName: "gain_down"; helpText: qsTr("Make the selected audio 1 dB quieter."); helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.") }
            AppMenuItem {
                text: qsTr("Normalize to −1 dBFS"); actionName: "normalize"
                helpText: qsTr("Raise or lower the selection so its loudest sample reaches −1 dBFS, the level 1 dB below the maximum a file can hold.")
            }
            AppMenuItem {
                text: qsTr("Insert Silence…"); actionName: "show_insert_silence"
                helpText: qsTr("Insert a length of silence at the playhead, pushing later audio along.")
            }
            AppMenuItem {
                objectName: "openGenerate"; text: qsTr("Generate Tone or Noise…"); actionName: "show_generate"
                helpText: qsTr("Create a tone, sweep or noise as a new clip on the focused track.")
                helpScope: qsTr("Replaces the selected time on that track, or starts at the playhead.")
            }
            AppMenuItem {
                text: qsTr("Paste and Mix…"); actionName: "show_mix_paste"
                helpText: qsTr("Blend the clipboard audio into the existing audio at the playhead, with a gain for each.")
            }
            AppMenuItem {
                text: qsTr("Map Track Channels…"); actionName: "show_channel_map"
                helpText: qsTr("Re-route the active track's channels: swap sides, sum to mono, or set any mix of inputs to outputs.")
            }
            AppMenuItem {
                text: qsTr("Reverse Selection"); actionName: "reverse"
                helpText: qsTr("Play the selected audio backwards: the last sample first.")
                helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.")
                enabled: root.service !== null && !root.service.selectionEmpty && !root.service.sampleEditing
                reason: qsTr("Select a time range and wait for sample editing to finish.")
            }
            AppMenuItem {
                text: qsTr("Convert Track to Mono"); actionName: "convert_mono"
                helpText: qsTr("Sum the active track's channels into one.")
                helpScope: qsTr("Runs in the background. Undo restores the previous audio.")
                enabled: root.service !== null && root.service.loaded && !root.service.sampleEditing
                reason: root.service === null || !root.service.loaded ? qsTr("Open or import audio first.")
                                                                      : qsTr("Wait for the current sample edit to finish.")
            }
            AppMenuItem {
                text: qsTr("Convert Track to Stereo"); actionName: "convert_stereo"
                helpText: qsTr("Give the active track two channels; a mono track is copied to both sides.")
                helpScope: qsTr("Runs in the background. Undo restores the previous audio.")
                enabled: root.service !== null && root.service.loaded && !root.service.sampleEditing
                reason: root.service === null || !root.service.loaded ? qsTr("Open or import audio first.")
                                                                      : qsTr("Wait for the current sample edit to finish.")
            }
            AppMenuItem {
                text: qsTr("Cancel Sample Editing"); actionName: "cancel_sample_edit"
                helpText: qsTr("Stop the background conversion in progress. The track keeps its previous audio.")
                enabled: root.service !== null && root.service.sampleEditing
                reason: qsTr("No sample edit is running.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Pitch and Duration…"); actionName: "show_stretch"
                helpText: qsTr("Change the clip's duration or pitch.")
            }
            AppMenuItem {
                objectName: "openRemix"; text: qsTr("Fit Music to Length…"); actionName: "show_remix"
                helpText: qsTr("Make a music clip shorter or longer by skipping or repeating matching passages, keeping its opening and ending.")
                helpScope: qsTr("Replaces the clip with the fitted music. One Undo step restores the original.")
            }
            AppMenuItem {
                text: qsTr("Adjust Crossfade…"); actionName: "show_crossfade"
                helpText: qsTr("Change the length and shape of an existing crossfade between two clips.")
            }
            AppMenuItem {
                text: qsTr("Automation…"); actionName: "show_automation"
                helpText: qsTr("Record and edit changes to volume, pan and effect parameters over time.")
            }
            AppMenuItem {
                text: qsTr("Generate Ducking…"); actionName: "show_generated_ducking"
                helpText: qsTr("Analyse trigger tracks, preview volume points and apply editable ducking to a chosen track.")
                helpScope: qsTr("Analysis makes no changes. Apply points installs the proposed volume automation as one Undo step.")
            }
            AppMenuItem {
                text: qsTr("Slip Tool"); actionName: "timeline_slip"
                helpText: qsTr("Change which part of the source plays inside a clip by dragging inside it.")
                helpScope: qsTr("The clip stays in the same position and keeps its length.")
            }
            AppMenuItem {
                text: qsTr("Slip Source Earlier (10 ms)"); actionName: "slip_earlier"
                helpText: qsTr("Move the audio inside the clip 10 milliseconds earlier. The clip itself stays put.")
            }
            AppMenuItem {
                text: qsTr("Slip Source Later (10 ms)"); actionName: "slip_later"
                helpText: qsTr("Move the audio inside the clip 10 milliseconds later. The clip itself stays put.")
            }
            AppMenuItem {
                text: qsTr("Crossfade Join — Linear"); actionName: "crossfade_linear"
                helpText: qsTr("Blend the two clips either side of a join across the selection, fading each in a straight line.")
                helpScope: qsTr("Select a range across one join. Both clips need unused source audio for the blend.")
            }
            AppMenuItem {
                text: qsTr("Crossfade Join — Equal Power"); actionName: "crossfade_power"
                helpText: qsTr("Blend the two clips either side of a join across the selection with curves that keep the level steady.")
                helpScope: qsTr("Select a range across one join. Both clips need unused source audio for the blend.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "openClipProperties"; text: qsTr("Clip Properties…"); actionName: "show_clip_properties"
                helpText: qsTr("See the source, timing and rate of the clip under the playhead, and set its gain and fades.")
            }
            AppMenuItem {
                objectName: "openClipEnvelopes"; text: qsTr("Clip Envelopes…"); actionName: "show_clip_envelopes"
                helpText: qsTr("Edit volume and stereo pan points that travel with one clip, using positions measured from its start.")
                helpScope: qsTr("Edits only the reference clip, including when it belongs to a linked group. Track automation remains separate.")
            }
            AppMenuItem {
                objectName: "openClipEffects"; text: qsTr("Clip Effects…"); actionName: "show_clip_effects"
                helpText: qsTr("Add effects that process one clip, before its fades and envelopes, and travel with it.")
                helpScope: qsTr("Split pieces of a clip share its effects. Track and master effects remain separate.")
            }
        }
        AppMenu {
            objectName: "menu_Multitrack"; title: qsTr("Arrange"); shell: root.shell
            AppMenuItem {
                text: qsTr("Add Track"); actionName: "add_track"
                helpText: qsTr("Add an empty track below the others.")
            }
            AppMenuItem {
                text: qsTr("Duplicate Track"); actionName: "duplicate_track"
                helpText: qsTr("Copy the active track with its clips and settings.")
            }
            AppMenuItem {
                text: qsTr("Delete Track"); actionName: "remove_track"
                helpText: qsTr("Delete the selected tracks, or the active track, and their clips from the project.")
                helpScope: qsTr("Undo restores it.")
            }
            MenuSep {}
            AppMenuItem { text: qsTr("Mute Track"); actionName: "toggle_mute"; helpText: qsTr("Silence the active track in playback and export without removing anything."); helpScope: qsTr("Undo does not change mute; press it again.") }
            AppMenuItem { text: qsTr("Solo Track"); actionName: "toggle_solo"; helpText: qsTr("Hear only the soloed tracks. Solo several to compare them."); helpScope: qsTr("Undo does not change solo; press it again.") }
            AppMenuItem {
                text: qsTr("Routing…"); actionName: "show_routing"
                helpText: qsTr("Buses, aux sends and sidechain inputs for the active track.")
                helpTopic: qsTr("A bus is a track that other tracks feed. An aux send copies some of a track to a bus at a set level. A sidechain lets one track's signal control an effect on another, such as ducking music under a voice.")
            }
            AppMenuItem {
                objectName: "openSpeechAlign"; text: qsTr("Match Speech Timing…"); actionName: "show_speech_align"
                helpText: qsTr("Re-time a re-recorded line so it follows the timing of a reference clip, keeping its voice.")
                helpScope: qsTr("The aligned copy goes on a new track. The original clips are unchanged.")
            }
            MenuSep {}
            AppMenu {
                title: qsTr("Track Role"); shell: root.shell
                objectName: "menu_TrackRole"
                Repeater {
                    model: root.service ? root.service.trackRoles : []
                    AppMenuItem {
                        required property string modelData
                        text: Theme.roleLabel(modelData)
                        checkable: true
                        checked: root.shell && root.shell.focusedTrackData
                                 ? root.shell.focusedTrackData.role === modelData : false
                        helpText: qsTr("Give the active track this role. A role sets the track's colour and says what it carries; the sound is not changed.")
                        onTriggered: root.shell.setFocusedRole(modelData)
                    }
                }
            }
            AppMenu {
                title: qsTr("Track Height"); shell: root.shell
                objectName: "menu_TrackHeight"
                Repeater {
                    model: root.service ? root.service.trackHeights : []
                    AppMenuItem {
                        required property string modelData
                        text: Theme.heightLabel(modelData)
                        checkable: true
                        checked: root.shell && root.shell.focusedTrackData
                                 ? root.shell.focusedTrackData.height === modelData : false
                        helpText: qsTr("Show the active track at this height. Display only, saved with the project.")
                        onTriggered: root.shell.setFocusedHeight(modelData)
                    }
                }
                MenuSep {}
                AppMenuItem { text: qsTr("Taller"); actionName: "track_taller"; helpText: qsTr("Give the active track more height on screen."); helpScope: qsTr("Display only, saved with the project.") }
                AppMenuItem { text: qsTr("Shorter"); actionName: "track_shorter"; helpText: qsTr("Give the active track less height on screen."); helpScope: qsTr("Display only, saved with the project.") }
            }
            MenuSep {}
            AppMenuItem { text: qsTr("Move Track Up"); actionName: "move_track_up"; helpText: qsTr("Move the active track one place up the list."); helpScope: qsTr("Order changes nothing in the sound. One Undo step.") }
            AppMenuItem { text: qsTr("Move Track Down"); actionName: "move_track_down"; helpText: qsTr("Move the active track one place down the list."); helpScope: qsTr("Order changes nothing in the sound. One Undo step.") }
            AppMenuItem { text: qsTr("Focus Next Track"); actionName: "focus_next_track"; helpText: qsTr("Make the next track active: the track you edit with shortcuts and the Track Effects panel.") }
            AppMenuItem { text: qsTr("Focus Previous Track"); actionName: "focus_prev_track"; helpText: qsTr("Make the previous track active: the track you edit with shortcuts and the Track Effects panel.") }
            MenuSep {}
            AppMenuItem { text: qsTr("Add Marker"); actionName: "add_marker"; helpText: qsTr("Put a marker at the playhead. Double-click a marker to name it."); helpScope: qsTr("Markers are project-wide and can be exported as cue points.") }
            AppMenuItem { text: qsTr("Next Marker"); actionName: "next_marker"; helpText: qsTr("Move the playhead to the next marker.") }
            AppMenuItem { text: qsTr("Previous Marker"); actionName: "prev_marker"; helpText: qsTr("Move the playhead to the previous marker.") }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Project Properties…"); actionName: "show_session_properties"
                helpText: qsTr("The project sample rate and the default delivery type.")
            }
        }
        AppMenu {
            objectName: "menu_Effects"; title: qsTr("Effects"); shell: root.shell
            id: effectsMenu
            // The groups need the catalogue; opening the menu starts it. They are
            // inserted after the loading line (rebuildEffectsMenu, effectMenuInsertAt).
            onAboutToShow: if (root.service) root.service.listPlugins()
            AppMenuItem { text: qsTr("Add Effect…"); actionName: "show_plugins"; helpText: qsTr("Open the effect browser to add an installed plugin to the active track."); helpScope: qsTr("The new insert goes at the end of the track's chain.") }
            AppMenuItem {
                text: root.shell && root.shell.effectsDockVisible ? qsTr("Hide Track Effects") : qsTr("Show Track Effects")
                actionName: "show_rack"
                helpText: qsTr("Show or hide the right dock: track effects, sound, recipes, the inspector and the video reference.")
            }
            AppMenuItem {
                text: qsTr("Bypass This Track's Chain"); actionName: "bypass_track_plugins"
                helpText: qsTr("Turn the active track's effects off and on together without removing them.")
            }
            AppMenuItem {
                objectName: "menuHearWithoutEffects"
                text: qsTr("Hear Without Effects"); actionName: "hear_without_effects"
                checkable: true; checked: root.service !== null && root.service.hearWithoutEffects
                helpText: qsTr("Before and after: play the whole project with every effect off (tracks, clips and the master), then switch back to compare.")
                helpScope: qsTr("Listening only. The project, its effects and exports are unchanged.")
            }
            AppMenuItem {
                objectName: "repeatEffect"
                text: root.service && root.service.lastEffectName !== ""
                      ? qsTr("Apply %1 Again").arg(Theme.shortName(root.service.lastEffectName, 32)) : qsTr("Apply Last Effect Again")
                actionName: "repeat_effect"
                helpText: qsTr("Apply the effect you last applied to a selection again, with the same settings, to the selection now.")
                helpScope: qsTr("Written into the selected audio as one Undo step. Kept when you open another file.")
                enabled: root.service !== null && root.service.lastEffectName !== "" && !root.service.selectionEmpty
                reason: root.service && root.service.lastEffectName === ""
                        ? qsTr("Apply an effect to a selection first.") : qsTr("Select a time range first.")
            }
            MenuSep {}
            AppMenuItem {
                objectName: "effectsMenuLoading"
                visible: root.service !== null && !root.service.pluginsListed
                height: visible ? implicitHeight : 0
                text: qsTr("Finding installed effects…")
                helpText: qsTr("The installed effects are being read. They appear in this menu by group when that is done.")
                enabled: false
                reason: qsTr("The effects appear here by group once the installed plugins have been read.")
            }
            MenuSep {}
            AppMenu {
                objectName: "effectsEssentialMenu"; title: qsTr("Quick Fixes"); shell: root.shell
                AppMenuItem {
                    text: qsTr("Voice Cleanup…"); actionName: "show_voice_cleanup"
                    helpText: qsTr("Reduce background noise in speech with the built-in RNNoise model, at an amount you choose.")
                    helpScope: qsTr("Creates a cleaned copy. Undo restores the previous audio. No internet connection is needed.")
                }
                AppMenuItem {
                    text: qsTr("Dynamics…"); actionName: "show_essential_dynamics"
                    helpText: qsTr("Even out the level of speech with simple compressor controls, using an installed LV2 processor.")
                }
                AppMenuItem {
                    text: qsTr("Noise Gate…"); actionName: "show_essential_gate"
                    helpText: qsTr("Reduce sound below a threshold, such as noise between phrases, using an installed LV2 gate.")
                }
                AppMenuItem {
                    text: qsTr("De-Esser…"); actionName: "show_essential_deesser"
                    helpText: qsTr("Soften harsh s sounds in speech, using an installed LV2 processor.")
                }
                AppMenuItem {
                    text: qsTr("EQ…"); actionName: "show_essential_eq"
                    helpText: qsTr("Shape the tone of the active track with grouped equaliser controls.")
                }
                AppMenuItem {
                    text: qsTr("Brightness…"); actionName: "show_essential_clarity"
                    helpText: qsTr("Add presence to speech with harmonic enhancement controls.")
                }
                AppMenuItem {
                    text: qsTr("Pitch Correction…"); actionName: "show_essential_pitch"
                    helpText: qsTr("Move sung notes towards a chosen scale, gently or as an obvious effect, using the installed x42-Autotune.")
                }
                AppMenuItem {
                    text: qsTr("Ducking…"); actionName: "show_essential_ducking"
                    helpText: qsTr("Lower the active track automatically while a chosen trigger track plays, for example music under a voice.")
                }
            }
            AppMenu {
                objectName: "effectsRestorationMenu"; title: qsTr("Restoration"); shell: root.shell
                AppMenuItem {
                    text: qsTr("Change Selection Level (Spectral)"); actionName: "spectral_attenuate"; enabled: root.service !== null && !root.service.selectionEmpty && !root.service.repairing
                    helpText: qsTr("Lower the level of the selected time and frequency region by the amount set in the Repair panel.")
                    helpScope: qsTr("Uses the frequency band or mask on the Spectral page. One Undo step; the source is kept.")
                    reason: root.service === null || root.service.selectionEmpty ? qsTr("Select a time range first.")
                                                                                 : qsTr("Wait for the current repair to finish.")
                }
                AppMenuItem {
                    text: qsTr("Repair Selection (Spectral)"); actionName: "spectral_heal"; enabled: root.service !== null && !root.service.selectionEmpty && !root.service.repairing
                    helpText: qsTr("Replace the selected time and frequency region with an estimate from the audio around it.")
                    helpScope: qsTr("Uses the frequency band or mask on the Spectral page. One Undo step; the source is kept.")
                    reason: root.service === null || root.service.selectionEmpty ? qsTr("Select a time range first.")
                                                                                 : qsTr("Wait for the current repair to finish.")
                }
                AppMenuItem {
                    text: qsTr("Learn Noise from Selection"); actionName: "spectral_capture_print"; enabled: root.service !== null && !root.service.selectionEmpty
                    helpText: qsTr("Measure the selection as a sample of the noise, for Reduce Noise to subtract later.")
                    helpScope: qsTr("Select a stretch of nothing but the noise. The print is kept with the project.")
                    reason: qsTr("Select a time range first.")
                }
                AppMenuItem {
                    text: qsTr("Reduce Noise in Selection (Spectral)"); actionName: "spectral_denoise"; enabled: root.service !== null && !root.service.selectionEmpty && root.service.noisePrintReady && !root.service.repairing
                    helpText: qsTr("Subtract the learned noise sample from the selection, by the amount set in the Repair panel.")
                    reason: root.service === null || root.service.selectionEmpty ? qsTr("Select a time range first.")
                        : !root.service.noisePrintReady ? qsTr("Learn the noise first.")
                        : qsTr("Wait for the current repair to finish.")
                }
                MenuSep {}
                AppMenuItem {
                    text: qsTr("Find Clicks and Pops"); actionName: "spectral_find_clicks"; enabled: root.service !== null && root.service.trackCount > 0 && !root.service.detecting
                    helpText: qsTr("Scan the take for clicks and pops and list them in the Repair panel.")
                    helpScope: qsTr("Finding changes nothing. Repair the Clicks Found repairs them.")
                    reason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                                                                   : qsTr("A click scan is already running.")
                }
                AppMenuItem {
                    text: qsTr("Repair the Clicks Found"); actionName: "spectral_heal_clicks"; enabled: root.service !== null && root.service.clickCount > 0 && !root.service.repairing
                    helpText: qsTr("Repair every listed click by interpolating across it.")
                    helpScope: qsTr("One Undo step; the source is kept.")
                    reason: root.service === null || root.service.clickCount === 0 ? qsTr("Find clicks first.")
                                                                                   : qsTr("Wait for the current repair to finish.")
                }
                AppMenuItem {
                    text: qsTr("Find and Repair Clicks"); actionName: "spectral_auto_heal"; enabled: root.service !== null && root.service.trackCount > 0 && !root.service.detecting && !root.service.repairing
                    helpText: qsTr("Find clicks and pops and repair them in one step.")
                    reason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                        : root.service.detecting ? qsTr("A click scan is already running.")
                        : qsTr("Wait for the current repair to finish.")
                }
                MenuSep {}
                AppMenuItem {
                    text: qsTr("Hum Comb at 50 Hz (Spectral)"); actionName: "spectral_hum_50"; enabled: root.service !== null && root.service.trackCount > 0
                    helpText: qsTr("Select 50 Hz mains hum and its harmonics as a comb of narrow bands, over the selection or the whole take.")
                    helpScope: qsTr("This only selects the hum. Use Change Selection Level to reduce it, then listen to the result.")
                    reason: qsTr("Open or import audio first.")
                }
                AppMenuItem {
                    text: qsTr("Hum Comb at 60 Hz (Spectral)"); actionName: "spectral_hum_60"; enabled: root.service !== null && root.service.trackCount > 0
                    helpText: qsTr("Select 60 Hz mains hum and its harmonics as a comb of narrow bands, over the selection or the whole take.")
                    helpScope: qsTr("This only selects the hum. Use Change Selection Level to reduce it, then listen to the result.")
                    reason: qsTr("Open or import audio first.")
                }
                AppMenuItem {
                    text: qsTr("Clear Frequency Band"); actionName: "spectral_clear_band"; enabled: root.service !== null && !root.service.bandEmpty
                    helpText: qsTr("Remove the frequency band so repairs act on the whole spectrum of the selection.")
                    reason: qsTr("No frequency band is set.")
                }
                MenuSep {}
                AppMenuItem {
                    text: qsTr("New Mask Shapes Cut"); actionName: "spectral_mask_subtract"; checkable: true; checked: root.shell ? root.shell.spectralMaskSubtract : false
                    helpText: qsTr("New lasso, brush and harmonic shapes remove from the mask instead of adding to it.")
                }
                AppMenuItem {
                    text: qsTr("New Mask Shapes Keep"); actionName: "spectral_mask_intersect"; checkable: true; checked: root.shell ? root.shell.spectralMaskIntersect : false
                    helpText: qsTr("New shapes keep only the part of the mask they overlap.")
                }
                AppMenuItem {
                    text: qsTr("Undo Last Mask Shape"); actionName: "spectral_mask_undo"; enabled: root.service !== null && root.service.maskShapeCount > 0
                    helpText: qsTr("Remove the most recent shape from the mask.")
                    reason: qsTr("The mask has no shapes.")
                }
                AppMenuItem {
                    text: qsTr("Clear Mask"); actionName: "spectral_clear_mask"; enabled: root.service !== null && root.service.maskShapeCount > 0
                    helpText: qsTr("Remove every shape from the mask.")
                    reason: qsTr("The mask has no shapes.")
                }
                MenuSep {}
                AppMenuItem {
                    text: qsTr("Redraw Spectrogram"); actionName: "spectral_rebuild"; enabled: root.service !== null && root.service.trackCount > 0 && !root.service.spectralBuilding
                    helpText: qsTr("Analyse the audio again and redraw the spectrogram.")
                    reason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                                                                   : qsTr("The display is already being built.")
                }
                MenuSep {}
                AppMenuItem {
                    objectName: "openSpectralRepair"
                    text: qsTr("Spectral Editing Tools"); actionName: "workspace_spectral"
                    helpText: qsTr("Open the Spectral page to find and repair clicks, reduce noise and restore clipped audio.")
                }
            }
            AppMenu {
                objectName: "effectsAnalysisMenu"; title: qsTr("Analysis"); shell: root.shell
                AppMenuItem {
                    objectName: "openAnalysis"; text: qsTr("Level Statistics…"); actionName: "show_analysis"
                    helpText: qsTr("Measure peak, RMS, loudness, DC offset, clipping and dynamic range, and match clip loudness.")
                    helpScope: qsTr("The selection, or the focused track; as its clips play, before track effects.")
                }
                AppMenuItem {
                    objectName: "openFrequencyAnalysis"; text: qsTr("Spectrum…"); actionName: "show_frequency_analysis"
                    helpText: qsTr("Show the average spectrum of the selection, or of the focused track.")
                }
                AppMenuItem {
                    objectName: "openPhaseAnalysis"; text: qsTr("Stereo Phase…"); actionName: "show_phase_analysis"
                    helpText: qsTr("Show stereo correlation, balance and width: how the audio will sum to mono.")
                }
                MenuSep {}
                AppMenuItem {
                    text: qsTr("Analyse the Mix"); actionName: "analyse_master"; enabled: root.service !== null && root.service.trackCount > 0 && !root.service.analysing
                    helpText: qsTr("Measure the complete mix, including its effects, to check loudness and peak levels.")
                    reason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                                                                   : qsTr("An analysis is already running.")
                }
                AppMenuItem {
                    text: qsTr("Reset Loudness Meter"); actionName: "reset_loudness"
                    helpText: qsTr("Start the live loudness measurement again from nothing.")
                }
            }
            AppMenu {
                objectName: "effectsMasterMenu"; title: qsTr("Master Bus"); shell: root.shell
                AppMenuItem {
                    text: qsTr("Add Effect to Master Bus…"); actionName: "show_master_plugins"
                    helpText: qsTr("Insert an effect on the master bus, where the whole mix is processed once.")
                }
                AppMenuItem {
                    text: root.service && root.service.previewBefore ? qsTr("Hear the Mix After the Bus") : qsTr("Hear the Mix Before the Bus"); actionName: "preview_before"; enabled: root.service !== null && root.service.trackCount > 0
                    helpText: qsTr("Switch between the mix with and without the master bus effects.")
                    helpScope: qsTr("Level-matched once the mix has been analysed, so you compare the sound rather than the loudness.")
                    reason: qsTr("Open or import audio first.")
                }
            }
            // Recipes: saved lists of steps (the Recipes tab). Built in first,
            // then your own; a pick runs on the selection, or on the whole active
            // track when nothing is selected.
            AppMenu {
                id: recipesMenu
                objectName: "effectsRecipesMenu"; title: qsTr("Recipes"); shell: root.shell
                // The recipes themselves are inserted here (rebuildRecipesMenu).
                MenuSep {}
                AppMenuItem {
                    objectName: "repeatRecipe"
                    text: root.service && root.service.lastRecipeName !== ""
                          ? qsTr("Run %1 Again").arg(Theme.shortName(root.service.lastRecipeName, 32)) : qsTr("Run Last Recipe Again")
                    actionName: "repeat_recipe"
                    helpText: qsTr("Run the recipe you last ran again, on the selection now, or on the whole active track when nothing is selected.")
                    helpScope: qsTr("Written into the audio as one Undo step.")
                    enabled: root.service !== null && root.service.lastRecipeName !== "" && !root.service.recipeRunning
                    reason: root.service && root.service.recipeRunning ? qsTr("A recipe is running.") : qsTr("Run a recipe first.")
                }
                AppMenuItem {
                    objectName: "showRecipes"
                    text: qsTr("Show Recipes…"); actionName: "show_recipes"
                    helpText: qsTr("Open the Recipes tab in the right dock: run, change and make recipes.")
                }
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Set Track Loudness…"); actionName: "show_track_loudness"
                helpText: qsTr("Measure the active track's loudness and set its fader to reach a target, within the true-peak limit.")
            }
            AppMenuItem {
                objectName: "openPluginManager"; text: qsTr("Plug-in Manager…"); actionName: "show_plugin_manager"
                helpText: qsTr("Enable, disable and star the installed effects (LV2 and CLAP).")
            }
        }
        AppMenu {
            objectName: "menu_Favorites"; title: qsTr("Starred"); shell: root.shell
            AppMenuItem {
                objectName: "openPluginFavorites"; text: qsTr("Browse starred effects…"); actionName: "show_plugin_favorites"
                helpText: qsTr("Open the effect browser showing only the effects you starred.")
            }
            AppMenuItem {
                text: root.service ? qsTr("%1 starred effects").arg(root.service.favoritePluginUris.length) : qsTr("No starred effects")
                helpText: qsTr("How many effects you have starred. Starred effects are listed first in the effect browser.")
                enabled: false
                reason: qsTr("Use the star beside an effect to star or unstar it.")
            }
        }
        AppMenu {
            objectName: "menu_View"; title: qsTr("View"); shell: root.shell
            AppMenu {
                title: qsTr("Time Display"); shell: root.shell
                objectName: "menu_TimeDisplay"
                Repeater {
                    model: root.service ? root.service.timeDisplays : []
                    AppMenuItem {
                        required property string modelData
                        objectName: "timeDisplay_" + modelData
                        text: root.service ? root.service.timeDisplayLabel(modelData) : modelData
                        checkable: true
                        checked: root.shell ? root.shell.timeDisplay === modelData : false
                        helpText: qsTr("Write and read every position and length this way: the clock, the rulers, the read-outs and the fields in every panel.")
                        helpScope: qsTr("Display only; nothing in the project changes. Whatever is chosen, a field still understands 1.5s, 25ms and 1200smp.")
                        onTriggered: root.shell.timeDisplay = modelData
                    }
                }
            }
            MenuSep {}
            AppMenuItem { text: qsTr("Zoom In"); actionName: "zoom_in"; helpText: qsTr("Show less time across the window, in more detail."); helpScope: qsTr("The view only; nothing in the project changes.") }
            AppMenuItem { text: qsTr("Zoom Out"); actionName: "zoom_out"; helpText: qsTr("Show more time across the window."); helpScope: qsTr("The view only; nothing in the project changes.") }
            AppMenuItem { text: qsTr("Zoom to Fit"); actionName: "zoom_fit"; helpText: qsTr("Show the whole project across the window."); helpScope: qsTr("The view only; nothing in the project changes.") }
            AppMenuItem { text: qsTr("Zoom to Selection"); actionName: "zoom_selection"; helpText: qsTr("Show the selected time range across the window."); helpScope: qsTr("The view only. Select a time range first.") }
            AppMenuItem {
                objectName: "menuFillWindow"
                text: qsTr("Fill Window with the Waveform"); actionName: "fill_window"
                checkable: true; checked: root.shell ? root.shell.filledFrom !== null : false
                helpText: qsTr("Hide the side panels and the mixer so the waveform fills the window. Choose it again to put them back.")
                helpScope: qsTr("The view only; nothing in the project changes.")
            }
            MenuSep {}
            AppMenuItem {
                text: qsTr("Files"); checkable: true
                checked: root.shell ? root.shell.leftDockVisible : true
                helpText: qsTr("Show or hide the left dock: files, markers and history.")
                onTriggered: root.shell.leftDockVisible = !root.shell.leftDockVisible
            }
            AppMenuItem {
                text: qsTr("Mixer"); actionName: "show_mixer"; checkable: true
                checked: root.shell ? root.shell.mixerVisible : true
                helpText: qsTr("Show or hide the mixer with a channel strip for every track.")
            }
            AppMenuItem {
                text: qsTr("Track Effects"); checkable: true
                checked: root.shell ? root.shell.effectsDockVisible : true
                helpText: qsTr("Show or hide the right dock: effects, inspector, repair, loudness and video.")
                onTriggered: root.shell.runAction("show_rack")
            }
            AppMenuItem {
                objectName: "menuVideo"
                text: qsTr("Video Reference"); onTriggered: root.shell.runAction("show_video")
                helpText: qsTr("Show the reference picture and its timecode, frame rate and placement in the right dock's Video tab.")
            }
        }
        AppMenu {
            objectName: "menu_Window"; title: qsTr("Window"); shell: root.shell
            AppMenuItem { text: qsTr("Take"); actionName: "workspace_waveform"; checkable: true; checked: root.workspace === "Waveform"; helpText: qsTr("The Take page: one track's audio, for cutting, fading, gain and the sample edits.") }
            AppMenuItem { text: qsTr("Arrange"); actionName: "workspace_multitrack"; checkable: true; checked: root.workspace === "Multitrack"; helpText: qsTr("The Arrange page: every track, for arranging clips, mixing and automation.") }
            AppMenuItem { text: qsTr("Spectral"); actionName: "workspace_spectral"; checkable: true; checked: root.workspace === "Spectral"; helpText: qsTr("The Spectral page: time across and frequency up, for finding and repairing noises.") }
            AppMenuItem { text: qsTr("Mastering"); actionName: "workspace_mastering"; checkable: true; checked: root.workspace === "Mastering"; helpText: qsTr("The Mastering page: the master bus, loudness measurement and delivery.") }
            MenuSep {}
            AppMenuItem {
                objectName: "openAudioLoad"; text: qsTr("Audio Load…"); actionName: "show_audio_load"
                helpText: qsTr("See how much of each audio block playback needs, any dropouts, the heaviest effects, and how many tracks this computer can mix.")
            }
            AppMenuItem {
                objectName: "menuAudioSetup"
                text: qsTr("Audio Setup…"); onTriggered: root.shell.runAction("show_audio_setup")
                helpText: qsTr("Outputs, inputs, sound cards, rates and JACK clients as the audio system reports them, and what to check when there is no sound.")
            }
            AppMenuItem {
                text: qsTr("Reset Layout"); onTriggered: root.shell.resetLayout()
                helpText: qsTr("Return the docks and panels to their default sizes and visibility.")
            }
        }
        AppMenu {
            objectName: "menu_Help"; title: qsTr("Help"); shell: root.shell
            AppMenuItem {
                objectName: "menuGettingStarted"
                text: qsTr("Getting Started"); onTriggered: root.shell.showGettingStarted()
                helpText: qsTr("Open the guides: getting started, importing, recording, editing, effects, spectral repair, saving and exporting.")
                helpScope: qsTr("Also brings back the welcome card on an empty project.")
            }
            AppMenuItem {
                text: qsTr("Control Help"); actionName: "show_control_help"
                helpText: qsTr("Explain the control under the pointer or with keyboard focus, and list every explained control.")
            }
            AppMenuItem {
                objectName: "menuUserManual"
                text: qsTr("User Manual"); actionName: "show_manual"
                helpText: qsTr("Find step-by-step help for editing, recording, repairs and export, plus shortcuts and explanations of audio terms.")
                helpScope: qsTr("Works offline. Use the contents or search to find a task or unfamiliar word.")
            }
            AppMenuItem {
                objectName: "menuKeyboardShortcuts"
                text: qsTr("Keyboard Shortcuts"); onTriggered: root.shell.showKeymapHelp()
                helpText: qsTr("Open the manual at the table of every keyboard shortcut, and say where a keymap of your own would go.")
                helpScope: qsTr("Every menu also shows its own binding beside it.")
            }
            AppMenuItem { text: qsTr("About OmaAudio"); onTriggered: root.shell.about(); helpText: qsTr("The version, the licence and the notices for the components OmaAudio uses.") }
        }
    }

    // ── workspaces ──────────────────────────────────────────────────────
    // Centred when there is room; at the 1280 px minimum the menus need the
    // space, so the tabs slide right rather than sit under "Favorites".
    WorkspaceBar {
        id: nav
        visible: menuBar.x + menuBar.width + Theme.s4 + width + Theme.s3 <= actions.x
        x: Math.min(Math.max(menuBar.x + menuBar.width + Theme.s4, (root.width - width) / 2),
                    actions.x - width - Theme.s3)
        height: parent.height
        onActivated: function (name) { root.workspaceActivated(name) }
    }

    ComboField {
        objectName: "compactWorkspace"
        helpTitle: qsTr("Workspace")
        helpText: qsTr("Switch between the Take, Arrange, Spectral and Mastering pages.")
        visible: !nav.visible
        anchors.right: actions.left
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        width: 118
        readonly property var pages: ["Waveform", "Multitrack", "Spectral", "Mastering"]
        model: pages.map(name => ({ name: name, label: Pages.label(name) }))
        textRole: "label"
        currentIndex: Math.max(0, pages.indexOf(root.workspace))
        onActivated: function(i) { root.workspaceActivated(pages[i]) }
    }

    // ── document actions + window controls ──────────────────────────────
    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
        // The two placeholders give way first when the bar is tight.
        readonly property bool roomy: root.width >= 1400

        IconButton {
            objectName: "actionSave"
            iconName: "save"
            readonly property bool dirty: root.service !== null && root.service.dirty
            text: dirty ? qsTr("Save (unsaved changes)") : qsTr("Save")
            helpText: qsTr("Write the project to its project bundle.")
            helpScope: qsTr("The first save asks where to put the project.")
            shortcut: root.shell ? root.shell.bindingFor("save") : ""
            iconColor: dirty ? Theme.accent : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("save")
        }
        IconButton {
            objectName: "actionUndo"
            iconName: "undo-2"; text: qsTr("Undo")
            helpText: qsTr("Reverse the last edit.")
            unavailableReason: qsTr("Nothing to undo.")
            shortcut: root.shell ? root.shell.bindingFor("undo") : ""
            enabled: root.service !== null && root.service.canUndo
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("undo")
        }
        IconButton {
            objectName: "actionRedo"
            iconName: "redo-2"; text: qsTr("Redo")
            helpText: qsTr("Apply again the edit you last undid.")
            unavailableReason: qsTr("Nothing to redo.")
            shortcut: root.shell ? root.shell.bindingFor("redo") : ""
            enabled: root.service !== null && root.service.canRedo
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("redo")
        }
        IconButton {
            iconName: "layout-grid"; text: qsTr("Reset workspace layout")
            helpText: qsTr("Return the docks and panels to their default sizes and visibility.")
            visible: actions.roomy
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.resetLayout()
        }
        IconButton {
            iconName: "speaker"; text: qsTr("Recording inputs…")
            helpText: qsTr("Open the Recording panel to choose inputs, channels and timing.")
            onClicked: root.shell.runAction("show_recording")
            visible: actions.roomy
            anchors.verticalCenter: parent.verticalCenter
        }
        IconButton {
            objectName: "preferencesButton"
            iconName: "settings"; text: qsTr("Preferences…")
            helpText: qsTr("Editing, recovery, spectral display and delivery defaults.")
            onClicked: root.shell.runAction("show_preferences")
            visible: actions.roomy
            anchors.verticalCenter: parent.verticalCenter
        }

        Item { width: Theme.s2; height: 1 }

        IconButton {
            objectName: "winMinimise"
            iconName: "minus"; text: qsTr("Minimise")
            helpText: qsTr("Minimise the window. Playback and background work continue.")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: if (root.target) root.target.showMinimized()
        }
        IconButton {
            objectName: "winMaximise"
            iconName: "square"; text: qsTr("Maximise")
            helpText: qsTr("Switch between a maximised and a windowed layout. Double-clicking the title bar does the same.")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: if (root.target)
                root.target.visibility = root.target.visibility === Window.Maximized
                                         ? Window.Windowed : Window.Maximized
        }
        IconButton {
            objectName: "winClose"
            iconName: "x"; text: qsTr("Close")
            helpText: qsTr("Close OmaAudio. If the project has unsaved changes you are asked whether to save them first.")
            anchors.verticalCenter: parent.verticalCenter
            iconColor: hovered ? Theme.danger : Theme.textSecondary
            onClicked: if (root.target) root.target.close()
        }
    }
}
