pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Broadcast metadata for the project and what the next render will write.
// The fields are the project's own (EBU bext); imported files keep theirs as
// provenance, shown in the Inspector, and are never merged in. Apply is one
// Undo step. The preview says exactly what goes into the file and what does not.
C.Popup {
    id: root
    objectName: "metadataPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string message: ""
    readonly property var preview: service && shell ? service.exportPreview(shell.deliverySpec("")) : ({})

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(660, parent ? parent.height - 32 : 660)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: { reload(); message = ""; description.forceActiveFocus(Qt.TabFocusReason) }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    function reload() {
        const m = service ? service.projectMetadata() : {}
        description.text = m.description || ""
        originator.text = m.originator || ""
        reference.text = m.originatorReference || ""
        date.text = m.originationDate || ""
        time.text = m.originationTime || ""
        timeRef.text = m.timeReferenceSet ? m.timeReferenceText : ""
        history.text = m.codingHistory || ""
    }
    function apply() {
        if (!service) return
        const ok = service.setProjectMetadata({
            description: description.text, originator: originator.text, originatorReference: reference.text,
            originationDate: date.text, originationTime: time.text,
            timeReferenceText: timeRef.text, codingHistory: history.text })
        message = ok ? qsTr("Metadata applied. Undo restores the previous values.") : service.lastError
        if (ok) reload()
    }

    component Field: C.TextField {
        property string help: ""
        property string role: ""
        readonly property alias controlHelp: fieldHelp
        Layout.fillWidth: true; selectByMouse: true; color: Theme.textPrimary
        // The theme's type and height, as every other field (TextEntry).
        implicitHeight: Theme.hControl; verticalAlignment: Text.AlignVCenter
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        padding: 0; leftPadding: Theme.s2; rightPadding: Theme.s2
        placeholderTextColor: Theme.textMuted; selectionColor: Theme.accent; selectedTextColor: Theme.accentText
        background: Rectangle { color: Theme.controlBg; radius: Theme.rControl; border.width: Theme.hairline; border.color: activeFocus ? Theme.accent : Theme.border }
        ControlHelp { id: fieldHelp; title: parent.role; purpose: parent.help; scope: qsTr("Written to BWF exports, and to WAV and RF64 when metadata is included. Nothing changes until you press Apply.") }
    }
    component Label: Text { Layout.preferredWidth: 150; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    component Note: Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Metadata"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { objectName: "metadataClose"; iconName: "x"; text: qsTr("Close"); onClicked: root.close(); helpText: qsTr("Close without applying draft changes.") }
        }
        Note { text: qsTr("The project's own broadcast description (EBU bext). Files you imported keep their own metadata as provenance, shown in the Inspector; it is never merged into these fields.") }

        ScrollPane {
            id: metadataScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
            ColumnLayout {
                x: metadataScroll.reachLeft; width: metadataScroll.columnWidth
                spacing: Theme.s2
                RowLayout { Label { text: qsTr("Description") } Field { id: description; objectName: "metadataDescription"; role: qsTr("Description"); help: qsTr("What the file is, in up to 256 characters: programme, episode, take.") } }
                RowLayout { Label { text: qsTr("Originator") } Field { id: originator; objectName: "metadataOriginator"; role: qsTr("Originator"); help: qsTr("Who made it: a person, studio or organisation, up to 32 characters.") } }
                RowLayout { Label { text: qsTr("Originator reference") } Field { id: reference; objectName: "metadataReference"; role: qsTr("Originator reference"); help: qsTr("An unambiguous reference from the originator, up to 32 characters, for example a job or catalogue number.") } }
                RowLayout {
                    Label { text: qsTr("Origination date, time") }
                    Field { id: date; objectName: "metadataDate"; role: qsTr("Origination date"); help: qsTr("yyyy-mm-dd. Empty writes the export's own date."); placeholderText: qsTr("export date") }
                    Field { id: time; objectName: "metadataTime"; role: qsTr("Origination time"); help: qsTr("hh:mm:ss. Empty writes the export's own time."); placeholderText: qsTr("export time") }
                    ToolButton { objectName: "metadataSetNow"; text: qsTr("Set to now"); showLabel: true
                        helpText: qsTr("Fill the date and time with the current local time.")
                        onClicked: { const now = new Date(); date.text = Qt.formatDate(now, "yyyy-MM-dd"); time.text = Qt.formatTime(now, "HH:mm:ss") } }
                }
                RowLayout {
                    Label { text: qsTr("Time reference") }
                    Field { id: timeRef; objectName: "metadataTimeRef"; role: qsTr("Time reference"); help: qsTr("Where the file starts in the day, as hh:mm:ss.mmm since midnight, or a number of samples at the project rate. Empty means none: the export starts at 0."); placeholderText: qsTr("hh:mm:ss.mmm or samples, empty for none") }
                }
                Note { text: qsTr("The exported file uses the project time reference shown here. Imported files keep their own references separately. To place imported audio at its recorded time, use import placement in the Files panel.") }
                RowLayout {
                    Label { text: qsTr("Coding history") }
                    Field { id: history; objectName: "metadataHistory"; role: qsTr("Coding history"); help: qsTr("Any history you want to keep, one EBU line per stage. The export appends its own line (A=PCM, F=rate, W=bits, M=channels, T=OmaAudio).") }
                }

                Text { Layout.topMargin: Theme.s2; text: qsTr("WHAT THE NEXT EXPORT WRITES"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading }
                RowLayout {
                    Toggle {
                        objectName: "metadataInclude"
                        label: qsTr("Include metadata in WAV and RF64")
                        checked: root.shell ? root.shell.deliveryIncludeMetadata : false
                        onToggled: if (root.shell) root.shell.setDelivery("deliveryIncludeMetadata", checked)
                        helpText: qsTr("Include these descriptive details in WAV and RF64 files. BWF includes them automatically.")
                        helpScope: qsTr("Compressed formats carry none of this. FLAC and AIFF carry none here either.")
                    }
                }
                RowLayout {
                    Label { text: qsTr("Markers") }
                    ComboField {
                        id: markers
                        objectName: "metadataMarkers"
                        caption: qsTr("Export")
                        Layout.preferredWidth: 300
                        model: [qsTr("None"), qsTr("Project markers"), qsTr("Source file markers")]
                        currentIndex: root.shell ? ["none", "project", "source"].indexOf(root.shell.deliveryMarkers) : 0
                        onActivated: index => { if (root.shell) root.shell.setDelivery("deliveryMarkers", ["none", "project", "source"][index]) }
                        helpText: qsTr("Which markers become cue points in the file: none, the project's markers at their timeline positions, or the markers carried by the source files, mapped through the clips that use them.")
                        helpScope: qsTr("Ranges are written with their length. A source marker not covered by a clip is left out. Positions are converted to the export rate.")
                    }
                }
                Text {
                    objectName: "metadataPreview"
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: Theme.textPrimary; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                    text: root.preview && root.preview.text ? root.preview.text : ""
                }
            }
        }
        Note { objectName: "metadataMessage"; text: root.message; visible: text !== "" }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton { objectName: "metadataReload"; text: qsTr("Reload"); showLabel: true; onClicked: root.reload(); helpText: qsTr("Discard draft changes and show the project's current metadata.") }
            ToolButton { objectName: "metadataApply"; text: qsTr("Apply"); showLabel: true; onClicked: root.apply()
                enabled: root.service !== null
                helpText: qsTr("Store these fields in the project as one Undo step. Save the project to keep them.") }
        }
    }
}
