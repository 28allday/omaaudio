#pragma once

#include "trackidmodel.h"

// The one object QML talks to.
//
// This is where the two processes became one (APP-PORT-PLAN.md, amendment A12). What
// used to be `Service.qml` marshalling a line protocol over a unix socket is now direct
// calls into the engine library: `session/`, `engine/`, `lv2/` and `io/` are linked in
// and unchanged, and nothing below this file knows Qt exists.
//
// THREADING:
//
//   * the GUI thread owns the model and everything here;
//   * the audio thread reads an immutable RenderPlan by borrowed pointer and talks back
//     through MeterChannel, which a timer here polls;
//   * one worker thread at a time does the slow things -- decoding a file, building a
//     peak pyramid, writing a project -- and hands its result back through `workerDone`,
//     which is a queued signal and therefore lands on the GUI thread.
//   * independent-file batches have a separate worker and immutable settings;
//     the GUI creates and releases their private plugin instances after joining.
//   * recording has a separate capture callback and disk writer, connected by fixed
//     SPSC rings; the GUI quiesces capture before installing the completed take.

#include "engine/meter_channel.h"
#include "lv2/chain.h"
#include "lv2/host_features.h"
#include "lv2/urid.h"
#include "lv2/world.h"
#include "video/reference.h"
#include "session/command.h"
#include "session/effect_owner.h"
#include "session/selection.h"
#include "engine/pw_output.h"
#include "engine/pw_capture.h"
#include "engine/recording.h"
#include "pluginlibrary.h"
#include "pluginpresets.h"
#include "displaycache.h"
#include "spectralstore.h"
#include "timeformat.h"
#include "recentdocuments.h"
#include "filebatch.h"
#include "engine/render_plan.h"
#include "engine/transport.h"
#include "io/peak_pyramid.h"
#include "io/peak_texture.h"
#include "render/export.h"
#include "render/master_render.h"
#include "render/ducking.h"
#include "render/clip_effects.h"
#include "plugineditor.h"
#include "io/project_file.h"
#include "session/project.h"
#include "session/sample_edit.h"
#include "session/channel_edit.h"
#include "session/multitrack_clipboard.h"
#include "session/project_rate.h"
#include "spectral/detect.h"
#include "session/types.h"
#include "spectral/repair.h"
#include "spectral/restoration.h"
#include "spectral/stft.h"
#include "spectral/tiles.h"

#include <QFile>
#include <QHash>
#include <QObject>
#include <QFont>
#include <QColor>
#include <QString>
#include <QVariantList>
#include <QUrl>
#include <QTimer>
#include <QImage>
#include <QElapsedTimer>
#include <QThreadPool>

class QProcess;
class AudioCommand;
class AudioLinks;

class QFileSystemWatcher;

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

// The engine library is namespaced; the app is not, and one using-directive at the
// top is clearer than `omawave::` on every third line of a file that talks to nothing
// else.
using namespace omawave;

// "hh:mm:ss.mmm" or a plain sample count; -1 when neither (backend.cpp).
[[nodiscard]] qint64 parseTimeReference(const QString &text, int rate);

class Backend : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY takeChanged)
    Q_PROPERTY(int defaultExportBits READ defaultExportBits NOTIFY projectSampleSettingsChanged)
    Q_PROPERTY(bool storageBusy READ storageBusy NOTIFY storageChanged)
    Q_PROPERTY(QString storageMessage READ storageMessage NOTIFY storageChanged)
    Q_PROPERTY(QVariantList unusedAudioFiles READ unusedAudioFiles NOTIFY storageChanged)
    Q_PROPERTY(QString storagePreviewStamp READ storagePreviewStamp NOTIFY storageChanged)
    Q_PROPERTY(bool elasticPreparing READ elasticPreparing NOTIFY statusChanged)
    Q_PROPERTY(bool elasticFailed READ elasticFailed NOTIFY statusChanged)
    Q_PROPERTY(qulonglong streamCacheMisses READ streamCacheMisses NOTIFY statusChanged)
    Q_PROPERTY(qulonglong streamReadErrors READ streamReadErrors NOTIFY statusChanged)
    // The memory budget (resourcebudget.h). memoryMiB is the setting, zero for
    // automatic; the rest say what this machine makes of it.
    Q_PROPERTY(int systemMemoryMiB READ systemMemoryMiB CONSTANT)
    Q_PROPERTY(int maximumMemoryMiB READ maximumMemoryMiB CONSTANT)
    Q_PROPERTY(int memoryMiB READ memoryMiB WRITE setMemoryMiB NOTIFY memoryChanged)
    Q_PROPERTY(int effectiveMemoryMiB READ effectiveMemoryMiB NOTIFY memoryChanged)
    Q_PROPERTY(int playbackCacheMiB READ playbackCacheMiB NOTIFY memoryChanged)
    // How positions and lengths are written and read everywhere (timeformat.h):
    // "time", "samples" or a timecode rate. The setting lives in the interface.
    Q_PROPERTY(QString timeDisplay READ timeDisplay WRITE setTimeDisplay NOTIFY timeDisplayChanged)
    Q_PROPERTY(QStringList timeDisplays READ timeDisplays CONSTANT)
    Q_PROPERTY(QString timeDisplayHint READ timeDisplayHint NOTIFY timeDisplayChanged)
    Q_PROPERTY(qint64 timeStep READ timeStep NOTIFY timeDisplayChanged)
    Q_PROPERTY(QString durationDisplayHint READ durationDisplayHint NOTIFY timeDisplayChanged)
    Q_PROPERTY(QVariantMap musicalTime READ musicalTime NOTIFY musicalTimeChanged)
    // Finished spectral displays kept on disk between sessions (spectralstore.h):
    // the most the folder may hold, and what it holds now.
    Q_PROPERTY(int spectralStoreGiB READ spectralStoreGiB WRITE setSpectralStoreGiB NOTIFY spectralStoreChanged)
    Q_PROPERTY(QString spectralStoreUsage READ spectralStoreUsage NOTIFY spectralStoreChanged)
    Q_PROPERTY(QString spectralStoreStatus READ spectralStoreStatus NOTIFY spectralStoreChanged)
    Q_PROPERTY(bool spectralStoreBusy READ spectralStoreBusy NOTIFY spectralStoreChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY takeChanged)
    Q_PROPERTY(QString takeName READ takeName NOTIFY takeChanged)
    Q_PROPERTY(QString takePath READ takePath NOTIFY takeChanged)
    Q_PROPERTY(qint64 takeFrames READ takeFrames NOTIFY timelineChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY timelineChanged)

    Q_PROPERTY(int playbackLatencyFrames READ playbackLatencyFrames NOTIFY statusChanged)
    // The PipeWire graph's clock rate while playing, 0 when unknown; the project
    // rate is what the stream runs at and PipeWire converts between the two.
    Q_PROPERTY(int graphSampleRate READ graphSampleRate NOTIFY statusChanged)
    // The Audio Setup picture (see audiosetup.h), refreshed on request; never
    // changes a default, opens a device or starts monitoring.
    Q_PROPERTY(QVariantMap audioSetup READ audioSetup NOTIFY audioSetupChanged)
    Q_PROPERTY(QVariantMap videoInfo READ videoInfo NOTIFY videoChanged)
    Q_PROPERTY(bool videoProbing READ videoProbing NOTIFY videoChanged)
    Q_PROPERTY(QString videoTimecode READ videoTimecode NOTIFY videoTimecodeChanged)
    Q_PROPERTY(QVariantMap videoStats READ videoStats NOTIFY videoTimecodeChanged)
    // Where an imported file lands: "start", "playhead" or "timestamp" (its bext
    // time reference against the project's; see metadata.h and the W05 plan).
    Q_PROPERTY(QString importPlacement READ importPlacement WRITE setImportPlacement NOTIFY importPlacementChanged)
    Q_PROPERTY(bool audioSetupScanning READ audioSetupScanning NOTIFY audioSetupChanged)
    Q_PROPERTY(bool audioLinkBusy READ audioLinkBusy NOTIFY audioSetupChanged)
    Q_PROPERTY(QString audioLinkMessage READ audioLinkMessage NOTIFY audioSetupChanged)
    Q_PROPERTY(bool primingPlayback READ primingPlayback NOTIFY statusChanged)
    Q_PROPERTY(bool unsupportedPlaybackLatency READ unsupportedPlaybackLatency NOTIFY statusChanged)
    Q_PROPERTY(qint64 playhead READ playhead NOTIFY statusChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY statusChanged)
    // Shuttle speed while playing: 1 is normal, 2/4/8 faster, negative backwards.
    Q_PROPERTY(int playSpeed READ playSpeed NOTIFY statusChanged)
    Q_PROPERTY(bool loopEnabled READ loopEnabled NOTIFY loopChanged)
    Q_PROPERTY(qint64 loopStart READ loopStart NOTIFY loopChanged)
    Q_PROPERTY(qint64 loopEnd READ loopEnd NOTIFY loopChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingStatus READ recordingStatus NOTIFY recordingChanged)
    Q_PROPERTY(qint64 recordingFrames READ recordingFrames NOTIFY recordingChanged)
    Q_PROPERTY(qint64 recordingStart READ recordingStart NOTIFY recordingChanged)
    Q_PROPERTY(double inputPeak READ inputPeak NOTIFY recordingChanged)
    Q_PROPERTY(int recordArmedTrack READ recordArmedTrack WRITE setRecordArmedTrack NOTIFY recordingChanged)
    Q_PROPERTY(QString recordInput READ recordInput WRITE setRecordInput NOTIFY recordingChanged)
    Q_PROPERTY(int recordChannels READ recordChannels WRITE setRecordChannels NOTIFY recordingChanged)
    Q_PROPERTY(bool recordMonitor READ recordMonitor WRITE setRecordMonitor NOTIFY recordingChanged)
    Q_PROPERTY(double recordPreRoll READ recordPreRoll NOTIFY recordingChanged)
    Q_PROPERTY(int recordCountIn READ recordCountIn NOTIFY recordingChanged)
    Q_PROPERTY(int recordLoopTakes READ recordLoopTakes NOTIFY recordingChanged)
    Q_PROPERTY(double recordTempo READ recordTempo NOTIFY recordingChanged)
    Q_PROPERTY(bool recordAlign READ recordAlign NOTIFY recordingChanged)
    Q_PROPERTY(double recordOffsetMs READ recordOffsetMs NOTIFY recordingChanged)
    Q_PROPERTY(qint64 recordAlignmentFrames READ recordAlignmentFrames NOTIFY recordingChanged)
    Q_PROPERTY(bool recordPunch READ recordPunch WRITE setRecordPunch NOTIFY recordingChanged)
    Q_PROPERTY(QStringList recordingInputNames READ recordingInputNames NOTIFY recordingInputsChanged)
    Q_PROPERTY(QStringList recordingInputIds READ recordingInputIds NOTIFY recordingInputsChanged)
    Q_PROPERTY(QVariantList additionalRecordInputs READ additionalRecordInputs NOTIFY recordingChanged)
    Q_PROPERTY(bool scanningInputs READ scanningInputs NOTIFY recordingInputsChanged)
    Q_PROPERTY(QVariantList recoverableRecordings READ recoverableRecordings NOTIFY recordingInputsChanged)
    Q_PROPERTY(bool clipMoving READ clipMoving NOTIFY clipMoveChanged)
    Q_PROPERTY(QVariantMap clipMovePreview READ clipMovePreview NOTIFY clipMoveChanged)
    Q_PROPERTY(double peakLeft READ peakLeft NOTIFY statusChanged)
    Q_PROPERTY(double peakRight READ peakRight NOTIFY statusChanged)

    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(int focusedTrack READ focusedTrack WRITE setFocusedTrack
                   NOTIFY tracksChanged)
    Q_PROPERTY(qint64 selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(qint64 selectionEnd READ selectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(qint64 selectionLength READ selectionLength NOTIFY selectionChanged)
    Q_PROPERTY(bool selectionEmpty READ selectionEmpty NOTIFY selectionChanged)
    Q_PROPERTY(int selectionTrack READ selectionTrack NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList selectedTracks READ selectedTracks NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList selectedClips READ selectedClips NOTIFY selectionChanged)
    // Tracks picked by their headers (not merely the focused lane).
    Q_PROPERTY(bool tracksPicked READ tracksPicked NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList clipGroups READ clipGroups NOTIFY tracksChanged)
    Q_PROPERTY(bool clipGroupsLinked READ clipGroupsLinked WRITE setClipGroupsLinked NOTIFY selectionChanged)
    Q_PROPERTY(int selectionChannel READ selectionChannel WRITE setSelectionChannel NOTIFY selectionChanged)
    Q_PROPERTY(QString clipboardSummary READ clipboardSummary NOTIFY timelineChanged)

    Q_PROPERTY(bool dirty READ dirty NOTIFY timelineChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY timelineChanged)
    Q_PROPERTY(bool saveQueued READ saveQueued NOTIFY timelineChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY timelineChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY timelineChanged)
    Q_PROPERTY(QVariantList recentDocuments READ recentDocuments NOTIFY recentDocumentsChanged)
    Q_PROPERTY(QString recentDocumentsError READ recentDocumentsError NOTIFY recentDocumentsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY timelineChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY timelineChanged)

    // Autosave and recovery (amendment A22). A dirty session is written to the
    // instance's private recovery bundle `autosaveSeconds` after the first change since the
    // last write (0 = never); a real save takes the layer away. What an earlier run
    // left behind is found at construction and offered once; the window asks.
    Q_PROPERTY(int autosaveSeconds READ autosaveSeconds WRITE setAutosaveSeconds NOTIFY autosaveChanged)
    // Read installed VST3 effects too. Takes effect when the isolated scanner helper
    // is installed beside the application; the next catalogue read includes them.
    Q_PROPERTY(bool vst3Discovery READ vst3Discovery WRITE setVst3Discovery NOTIFY pluginsChanged)
    Q_PROPERTY(bool vst3DiscoveryAvailable READ vst3DiscoveryAvailable CONSTANT)
    Q_PROPERTY(bool autosaving READ autosaving NOTIFY autosaveChanged)
    Q_PROPERTY(QString lastAutosave READ lastAutosave NOTIFY autosaveChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryChanged)
    Q_PROPERTY(QString recoveryName READ recoveryName NOTIFY recoveryChanged)
    Q_PROPERTY(QString recoveryWritten READ recoveryWritten NOTIFY recoveryChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY timelineChanged)
    Q_PROPERTY(bool anySolo READ anySolo NOTIFY tracksChanged)

    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsListed READ pluginsListed NOTIFY pluginsChanged)
    Q_PROPERTY(QStringList pluginScanWarnings READ pluginScanWarnings NOTIFY pluginsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY pluginsChanged)
    Q_PROPERTY(QStringList favoritePluginUris READ favoritePluginUris NOTIFY pluginLibraryChanged)
    Q_PROPERTY(QStringList recentPluginUris READ recentPluginUris NOTIFY pluginLibraryChanged)
    Q_PROPERTY(QStringList disabledPluginUris READ disabledPluginUris NOTIFY pluginLibraryChanged)
    // The Effects menu: the core set and every effect, by group, one entry per
    // family (see effectmenu.h).
    Q_PROPERTY(QVariantMap effectMenu READ effectMenu NOTIFY effectMenuChanged)
    Q_PROPERTY(QVariantList pluginPorts READ pluginPorts NOTIFY pluginPortsChanged)
    Q_PROPERTY(quint64 pluginValuesRevision READ pluginValuesRevision NOTIFY pluginValuesChanged)
    // Bumped when a measured frequency response (pluginResponse) arrives.
    Q_PROPERTY(quint64 pluginResponseRevision READ pluginResponseRevision NOTIFY pluginResponseChanged)
    Q_PROPERTY(quint64 pluginTextRevision READ pluginTextRevision NOTIFY pluginTextChanged)
    Q_PROPERTY(int pluginPortsSlot READ pluginPortsSlot NOTIFY pluginPortsChanged)
    Q_PROPERTY(bool pluginPortsComplete READ pluginPortsComplete
                   NOTIFY pluginPortsChanged)
    // The same slot's output controls, drawn as meters: [{index, symbol, name, min,
    // max, unit, group}] (latency reports left out); its kind for the panel layout
    // ("dynamics" | "eq" | "other", from the plugin's class); and its URI.
    Q_PROPERTY(QVariantList pluginOutputs READ pluginOutputs NOTIFY pluginPortsChanged)
    Q_PROPERTY(QString pluginPortsKind READ pluginPortsKind NOTIFY pluginPortsChanged)
    Q_PROPERTY(QString pluginPortsUri READ pluginPortsUri NOTIFY pluginPortsChanged)

    // Always true, always false: the QML asks because it used to talk to a separate
    // process that could be missing or disconnected. Kept rather than edited out of
    // fifteen hundred lines that mention them.
    Q_PROPERTY(bool connected READ connected CONSTANT)
    Q_PROPERTY(bool engineMissing READ engineMissing CONSTANT)

    Q_PROPERTY(int takeChannels READ takeChannels NOTIFY takeChanged)
    Q_PROPERTY(int originalSampleRate READ originalSampleRate NOTIFY takeChanged)
    Q_PROPERTY(bool resampled READ resampled NOTIFY takeChanged)
    Q_PROPERTY(int focused READ focusedTrack NOTIFY tracksChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList trackIds READ trackIds NOTIFY tracksChanged)
    // The same ids as an incremental list model, for views that create a delegate per
    // track: adding one track creates one delegate (see trackidmodel.h).
    Q_PROPERTY(QObject* trackIdModel READ trackIdModel CONSTANT)
    // Cheap things to bind to instead of `tracks`, whose every read converts the whole
    // list (all clips and waveform levels) for QML: a counter that changes with it,
    // and a light list of {id, name, frames, height} for pickers and layout.
    // Their own signal, emitted once they are rebuilt: QML bindings run on a signal
    // before ordinary slots, so notifying on tracksChanged would hand them stale data.
    Q_PROPERTY(int tracksRevision READ tracksRevision NOTIFY trackSummariesChanged)
    Q_PROPERTY(QVariantList trackSummaries READ trackSummaries NOTIFY trackSummariesChanged)

    Q_PROPERTY(int samplesPerPeak READ samplesPerPeak CONSTANT)
    Q_PROPERTY(int maxTracks READ maxTracks CONSTANT)

    // --- Phase 1 (Multitrack) ------------------------------------------------------

    // Session markers, sorted by position: {id, name, position, length, end, type,
    // color, notes}.
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)

    // Snapping: selection edges land on the nearest clip boundary, marker or the
    // playhead within `snapToleranceFrames` (the QML sets it from the zoom), and
    // otherwise on the optional musical grid or a zero crossing. Off means raw frames.
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY snappingChanged)
    Q_PROPERTY(qint64 snapToleranceFrames READ snapToleranceFrames
                   WRITE setSnapToleranceFrames NOTIFY snappingChanged)
    Q_PROPERTY(QString snapGrid READ snapGrid WRITE setSnapGrid NOTIFY snappingChanged)
    Q_PROPERTY(QStringList snapGrids READ snapGrids CONSTANT)
    Q_PROPERTY(qint64 snapGridTicks READ snapGridTicks NOTIFY snappingChanged)
    Q_PROPERTY(bool musicalSnapActive READ musicalSnapActive NOTIFY snappingChanged)
    Q_PROPERTY(QString snapHint READ snapHint NOTIFY snappingChanged)

    // Ripple: delete and cut close the gap, paste pushes later content along. Off
    // (the default, A13) leaves gaps and pastes over what is there.
    Q_PROPERTY(bool ripple READ ripple WRITE setRipple NOTIFY rippleChanged)

    // The undo history as a list: {index, label, applied}. `historyCursor` is how many
    // are applied; jumpHistory() moves it.
    Q_PROPERTY(QVariantList history READ history NOTIFY timelineChanged)
    Q_PROPERTY(int historyCursor READ historyCursor NOTIFY timelineChanged)

    // The monitor level, in dB. Not part of the mix (see session::mixdown).
    Q_PROPERTY(double masterGainDb READ masterGainDb NOTIFY masterGainChanged)
    // A click on each beat of the project tempo while playing or recording. Heard
    // only: not saved, not an Undo step, never in exports or measurements.
    Q_PROPERTY(bool metronome READ metronome WRITE setMetronome NOTIFY metronomeChanged)
    Q_PROPERTY(bool checkingComputer READ checkingComputer NOTIFY computerCheckChanged)
    Q_PROPERTY(QVariantMap computerCheck READ computerCheck NOTIFY computerCheckChanged)
    Q_PROPERTY(bool aligningSpeech READ aligningSpeech NOTIFY speechAlignmentChanged)
    Q_PROPERTY(bool remixing READ remixing NOTIFY remixChanged)
    Q_PROPERTY(QString remixMessage READ remixMessage NOTIFY remixChanged)
    Q_PROPERTY(QString speechAlignMessage READ speechAlignMessage NOTIFY speechAlignmentChanged)
    Q_PROPERTY(double metronomeLevelDb READ metronomeLevelDb WRITE setMetronomeLevelDb NOTIFY metronomeChanged)

    // Per-track peaks, post-fader and pre-master, in track order: {id, left, right}.
    Q_PROPERTY(QVariantList trackMeters READ trackMeters NOTIFY statusChanged)
    // Changes whenever track meters move: bind to this and call trackMeter(id), which
    // is a constant-time lookup, rather than reading the whole trackMeters list.
    Q_PROPERTY(int meterRevision READ meterRevision NOTIFY statusChanged)

    // The vocabularies, for combos and menus. Lower-case names as the model spells
    // them; the UI translates.
    // --- the master bus (Phase 3) ---
    // The bus chain in the same shape as a track's `inserts`; every plugin call
    // takes `masterTarget` as its track argument to address it.
    Q_PROPERTY(QVariantList masterInserts READ masterInserts NOTIFY masterChanged)
    Q_PROPERTY(qint64 masterLatency READ masterLatency NOTIFY masterChanged)
    Q_PROPERTY(double masterTrimDb READ masterTrimDb NOTIFY masterChanged)
    Q_PROPERTY(int masterTarget READ masterTarget CONSTANT)
    // Which chain the port descriptors belong to: masterTarget, or a track id.
    Q_PROPERTY(int pluginPortsTarget READ pluginPortsTarget NOTIFY pluginPortsChanged)
    // The live R128 meter on the mix, polled with the status.
    Q_PROPERTY(QVariantMap loudness READ loudness NOTIFY loudnessChanged)
    // Hear the mix before the bus, level-matched to the mastered mix by the
    // difference the last analysis measured. Monitor-side only: exports never see it.
    Q_PROPERTY(bool previewBefore READ previewBefore WRITE setPreviewBefore NOTIFY previewChanged)
    // Before / after for the whole project: playback without any effect (track,
    // clip and master), for comparing. Listening only: export, analysis renders and
    // the project are unchanged; a new or opened project starts with effects on.
    Q_PROPERTY(bool hearWithoutEffects READ hearWithoutEffects WRITE setHearWithoutEffects NOTIFY previewChanged)
    Q_PROPERTY(double previewMatchDb READ previewMatchDb NOTIFY previewChanged)
    Q_PROPERTY(bool previewMatched READ previewMatched NOTIFY previewChanged)
    // The offline analysis of the mastered mix: its waveform, its loudness history,
    // its report. Stale the moment anything about the mix changes.
    Q_PROPERTY(bool analysing READ analysing NOTIFY analysisChanged)
    Q_PROPERTY(bool measuringTrackLoudness READ measuringTrackLoudness NOTIFY trackLoudnessChanged)
    Q_PROPERTY(double trackLoudnessProgress READ trackLoudnessProgress NOTIFY trackLoudnessChanged)
    Q_PROPERTY(QVariantMap trackLoudnessReport READ trackLoudnessReport NOTIFY trackLoudnessChanged)
    Q_PROPERTY(bool analysed READ analysed NOTIFY analysisChanged)
    Q_PROPERTY(bool analysisStale READ analysisStale NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList masterLevels READ masterLevels NOTIFY analysisChanged)
    Q_PROPERTY(qint64 masterFrames READ masterFrames NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap masterReport READ masterReport NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList shortTermHistory READ shortTermHistory NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList momentaryHistory READ momentaryHistory NOTIFY analysisChanged)
    Q_PROPERTY(int historyHopFrames READ historyHopFrames NOTIFY analysisChanged)
    // Delivery.
    Q_PROPERTY(QVariantList exportFormats READ exportFormats CONSTANT)
    Q_PROPERTY(QVariantList deliveryPresets READ deliveryPresets CONSTANT)
    Q_PROPERTY(QVariantList renderQueue READ renderQueue NOTIFY renderQueueChanged)
    Q_PROPERTY(QVariantList fileBatchJobs READ fileBatchJobs NOTIFY fileBatchChanged)
    Q_PROPERTY(bool fileBatchBusy READ fileBatchBusy NOTIFY fileBatchChanged)
    Q_PROPERTY(QVariantMap fileBatchStatus READ fileBatchStatus NOTIFY fileBatchChanged)
    Q_PROPERTY(bool collecting READ collecting NOTIFY collectionChanged)
    Q_PROPERTY(QVariantMap collectionStatus READ collectionStatus NOTIFY collectionChanged)
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderQueueChanged)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderProgressChanged)

    // The Spectral page (Phase 4): the focused take's short-time spectrum as tiles the
    // page uploads, built on the worker; a frequency band beside the time selection;
    // the two repairs, which are the only edits that write samples.
    Q_PROPERTY(QVariantMap spectral READ spectral NOTIFY spectralChanged)
    Q_PROPERTY(bool spectralBuilding READ spectralBuilding NOTIFY spectralChanged)
    Q_PROPERTY(bool spectralStale READ spectralStale NOTIFY spectralChanged)
    Q_PROPERTY(bool spectralWanted READ spectralWanted WRITE setSpectralWanted NOTIFY spectralChanged)
    Q_PROPERTY(double spectralProgress READ spectralProgress NOTIFY spectralProgressChanged)
    Q_PROPERTY(int spectralFftSize READ spectralFftSize NOTIFY spectralConfigChanged)
    Q_PROPERTY(QString spectralWindow READ spectralWindow NOTIFY spectralConfigChanged)
    Q_PROPERTY(int spectralOverlap READ spectralOverlap NOTIFY spectralConfigChanged)
    Q_PROPERTY(QVariantList spectralFftSizes READ spectralFftSizes CONSTANT)
    Q_PROPERTY(QStringList spectralWindows READ spectralWindows CONSTANT)
    Q_PROPERTY(QVariantList spectralOverlaps READ spectralOverlaps CONSTANT)
    Q_PROPERTY(double bandLowHz READ bandLowHz NOTIFY selectionChanged)
    Q_PROPERTY(double bandHighHz READ bandHighHz NOTIFY selectionChanged)
    Q_PROPERTY(bool bandEmpty READ bandEmpty NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList maskShapes READ maskShapes NOTIFY maskChanged)
    Q_PROPERTY(bool maskEmpty READ maskEmpty NOTIFY maskChanged)
    Q_PROPERTY(int maskShapeCount READ maskShapeCount NOTIFY maskChanged)
    Q_PROPERTY(int maskFeather READ maskFeather WRITE setMaskFeather NOTIFY maskChanged)
    Q_PROPERTY(QString automationStamp READ automationStamp NOTIFY automationStampChanged)
    Q_PROPERTY(bool generatingDucking READ generatingDucking NOTIFY duckingChanged)
    Q_PROPERTY(double duckingProgress READ duckingProgress NOTIFY duckingChanged)
    Q_PROPERTY(QVariantMap duckingPreview READ duckingPreview NOTIFY duckingChanged)
    Q_PROPERTY(QString automationMode READ automationMode NOTIFY automationLiveChanged)
    Q_PROPERTY(int automationTrack READ automationTrack NOTIFY automationLiveChanged)
    Q_PROPERTY(QString automationKey READ automationKey NOTIFY automationLiveChanged)
    Q_PROPERTY(bool automationWriting READ automationWriting NOTIFY automationLiveChanged)
    Q_PROPERTY(double automationLiveValue READ automationLiveValue NOTIFY automationLiveChanged)
    Q_PROPERTY(QString automationLiveStatus READ automationLiveStatus NOTIFY automationLiveChanged)
    Q_PROPERTY(double automationReturnSeconds READ automationReturnSeconds WRITE setAutomationReturnSeconds NOTIFY automationReturnChanged)
    Q_PROPERTY(bool sampleEditing READ sampleEditing NOTIFY sampleEditChanged)
    Q_PROPERTY(bool sampleEditQueued READ sampleEditQueued NOTIFY sampleEditChanged)
    Q_PROPERTY(QString sampleEditLabel READ sampleEditLabel NOTIFY sampleEditChanged)
    // An effect being heard on the selection before it is applied there:
    // {active, applying, track, slot, uri, name, start, end}.
    Q_PROPERTY(QVariantMap effectPreview READ effectPreview NOTIFY effectPreviewChanged)
    // The effect last applied to a selection, for Apply Again ("" when none).
    Q_PROPERTY(QString lastEffectName READ lastEffectName NOTIFY effectPreviewChanged)
    // Recipes (backend_recipes.cpp).
    Q_PROPERTY(QVariantList recipes READ recipes NOTIFY recipesChanged)
    Q_PROPERTY(QVariantMap recipeRun READ recipeRun NOTIFY recipeRunChanged)
    Q_PROPERTY(bool recipeRunning READ recipeRunning NOTIFY recipeRunChanged)
    Q_PROPERTY(QString lastRecipeName READ lastRecipeName NOTIFY recipeRunChanged)
    Q_PROPERTY(bool recordingSteps READ recordingSteps NOTIFY recordedStepsChanged)
    Q_PROPERTY(QVariantList recordedSteps READ recordedSteps NOTIFY recordedStepsChanged)
    // Tidy pauses (backend_pauses.cpp): the pauses found, and the last tidy's result.
    Q_PROPERTY(QVariantMap pauses READ pauses NOTIFY pausesChanged)
    Q_PROPERTY(QVariantMap pauseResult READ pauseResult NOTIFY pausesChanged)
    Q_PROPERTY(bool repairing READ repairing NOTIFY repairChanged)
    Q_PROPERTY(bool repairQueued READ repairQueued NOTIFY repairChanged)
    Q_PROPERTY(QString repairSummary READ repairSummary NOTIFY repairChanged)
    Q_PROPERTY(QString repairChannel READ repairChannel WRITE setRepairChannel NOTIFY repairChanged)
    Q_PROPERTY(QString spectralView READ spectralView WRITE setSpectralView NOTIFY spectralConfigChanged)
    // Clicks and pops (A23): what the detector found on the focused take, as
    // [{frame, length, strength}] in the take's frames; gone when the take changes.
    Q_PROPERTY(QVariantList clicks READ clicks NOTIFY clicksChanged)
    Q_PROPERTY(int clickCount READ clickCount NOTIFY clicksChanged)
    Q_PROPERTY(bool detecting READ detecting NOTIFY clicksChanged)
    Q_PROPERTY(QString clickSummary READ clickSummary NOTIFY clicksChanged)
    Q_PROPERTY(bool noisePrintBuilding READ noisePrintBuilding NOTIFY noisePrintChanged)
    Q_PROPERTY(bool noisePrintReady READ noisePrintReady NOTIFY noisePrintChanged)
    Q_PROPERTY(QString noisePrintSummary READ noisePrintSummary NOTIFY noisePrintChanged)

    Q_PROPERTY(QStringList trackRoles READ trackRoles CONSTANT)
    Q_PROPERTY(QStringList trackHeights READ trackHeights CONSTANT)
    Q_PROPERTY(QStringList markerTypes READ markerTypes CONSTANT)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    // Starts the audio output. False with `lastError` set when there is no device --
    // the window still opens, because an editor that refuses to show you a file
    // because the speakers are busy is worse than one that cannot play it yet.
    bool start();

    // CLI completion includes queued worker results, not merely the worker flag.
    bool commandBusy();
    void synchronizeCommandState();
    quint32 commandPlaybackEpoch() const { return m_meters.playbackState().epoch; }
    bool commandTransportPending() const { return !m_commands.empty() || !m_pendingCommands.empty(); }
    void cancelCommandWork();
    bool prepareSessionCheckpoint(QString &index);

    [[nodiscard]] bool ready() const { return m_ready; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    [[nodiscard]] int sampleRate() const;
    [[nodiscard]] bool loading() const { return m_loading; }
    [[nodiscard]] bool loaded() const { return m_loaded; }
    [[nodiscard]] QString takeName() const { return m_takeName; }
    [[nodiscard]] QString takePath() const { return m_takePath; }
    [[nodiscard]] qint64 takeFrames() const;
    [[nodiscard]] double durationSeconds() const;

    [[nodiscard]] qint64 playhead() const { return m_playhead; }
    [[nodiscard]] bool playing() const { return m_playing; }
    [[nodiscard]] double peakLeft() const { return m_peakLeft; }
    [[nodiscard]] double peakRight() const { return m_peakRight; }

    [[nodiscard]] QVariantList tracks() const { return m_tracks; }
    [[nodiscard]] int playbackLatencyFrames() const { return static_cast<int>(std::min<std::uint32_t>(m_meters.latencyFrames(), 2147483647U)); }
    [[nodiscard]] bool primingPlayback() const { return m_meters.priming(); }
    [[nodiscard]] bool unsupportedPlaybackLatency() const { return m_meters.unsupportedLatency(); }
    [[nodiscard]] int focusedTrack() const { return static_cast<int>(m_focused); }
    void setFocusedTrack(int id);

    [[nodiscard]] int samplesPerPeak() const;
    [[nodiscard]] int maxTracks() const;

    // --- what the user can do ------------------------------------------------------

    [[nodiscard]] bool connected() const { return true; }
    [[nodiscard]] bool engineMissing() const { return false; }
    [[nodiscard]] int takeChannels() const { return m_takeChannels; }
    [[nodiscard]] int originalSampleRate() const { return m_originalRate; }
    [[nodiscard]] bool resampled() const { return m_resampled; }
    [[nodiscard]] int trackCount() const;
    [[nodiscard]] QVariantList trackIds() const;
    [[nodiscard]] QObject* trackIdModel() { return &m_trackIdModel; }
    [[nodiscard]] int tracksRevision() const { return m_tracksRevision; }
    [[nodiscard]] bool tracksPicked() const { return !m_selectedTracks.empty(); }
    [[nodiscard]] QVariantList trackSummaries() const { return m_trackSummaries; }

    // Lookups by id rather than by position: a track removed from the middle shifts
    // every index after it, and a lane holding an index would then draw the wrong one.
    // The keymap, as text. Parsed in QML because that is where the bindings are used,
    // and because a person edits the user file by hand -- a parse that is forgiving in
    // one place and strict in another would be worse than either.
    // Drops the last error so the status bar returns to its facts; QML and tests use it.
    Q_INVOKABLE void clearError();
    Q_INVOKABLE QString defaultKeymap() const;
    Q_INVOKABLE QString userKeymap() const;
    Q_INVOKABLE QString keymapFilePath() const { return userKeymapPath(); }
    int graphSampleRate() const { return static_cast<int>(m_output.graphRate()); }
    // The delivery capability model: can this spec be written, why not, and the
    // nearest choice that would work. Keys: ok, reason, suggestedSampleRate,
    // suggestedBitDepth, suggestedChannels, suggestedBitrateKbps (0 = none/automatic, -1 = no suggestion).
    Q_INVOKABLE QVariantMap checkDelivery(const QVariantMap &spec) const;
    [[nodiscard]] QVariantMap audioSetup() const { return m_audioSetup; }
    [[nodiscard]] bool audioSetupScanning() const { return m_audioSetupScan != nullptr; }
    Q_INVOKABLE void refreshAudioSetup();
    Q_INVOKABLE void cancelAudioSetupScan();
    [[nodiscard]] bool audioLinkBusy() const { return m_outputLinks != nullptr; }
    [[nodiscard]] QString audioLinkMessage() const { return m_audioLinkMessage; }
    Q_INVOKABLE void cancelAudioLinks();
    [[nodiscard]] QString importPlacement() const { return m_importPlacement; }
    void setImportPlacement(const QString &placement);
    // The project's broadcast metadata (keys as in session::ProjectMetadata plus
    // timeReferenceText); setting it is one Undo step.
    Q_INVOKABLE QVariantMap projectMetadata() const;
    Q_INVOKABLE bool setProjectMetadata(const QVariantMap &values);
    // What the focused (or given) track's single source said about itself.
    Q_INVOKABLE QVariantMap sourceMetadata(int track = -1) const;
    // What a render with this spec would write besides samples, and what it omits.
    Q_INVOKABLE QVariantMap exportPreview(const QVariantMap &spec) const;
    // "hh:mm:ss.mmm" since midnight for a sample count at a rate.
    Q_INVOKABLE static QString formatTimeReference(qint64 samples, int rate);

    // The reference picture (W11): one video file placed on the timeline by an
    // offset (the project frame where its time zero sits), decoded through the
    // FFmpeg boundary on its own thread, shown at whatever frame the audio
    // clock is on. Every change is one Undo step; the file is probed again
    // whenever the project's reference changes (open, undo, relink).
    Q_INVOKABLE bool importVideo(const QString &path);
    Q_INVOKABLE bool relinkVideo(const QString &path);
    Q_INVOKABLE bool clearVideo();
    Q_INVOKABLE bool setVideoOffset(qint64 frames);
    // "hh:mm:ss.mmm", seconds ("12.5"), a sample count, or any of those with a
    // leading minus for a picture that starts before the project.
    Q_INVOKABLE bool setVideoOffsetText(const QString &text);
    Q_INVOKABLE bool setVideoStartAtPlayhead();
    // The picture's timecode against the project's time reference (W05): with
    // both known the picture is placed by them; without a project reference
    // the picture's timecode becomes it and the picture sits at zero.
    Q_INVOKABLE bool alignVideoToTimeReference();
    Q_INVOKABLE bool setVideoDropFrame(bool on);
    // Move the playhead by whole picture frames (their own timestamps).
    Q_INVOKABLE bool videoStep(int frames);
    Q_INVOKABLE QString videoTimecodeAt(qint64 playhead) const;
    [[nodiscard]] QVariantMap videoInfo() const;
    [[nodiscard]] bool videoProbing() const { return m_videoProbing; }
    [[nodiscard]] QString videoTimecode() const { return m_videoTimecode; }
    [[nodiscard]] QVariantMap videoStats() const;
    // The frame on screen; the frame item paints it. watchVideo(+1/-1) says
    // whether anyone is looking, which is what runs the frame timer.
    [[nodiscard]] const QImage &videoFrame() const { return m_videoFrame; }
    [[nodiscard]] double videoFrameSeconds() const { return m_videoFrameSeconds; }
    void watchVideo(int delta);
    // Route OmaAudio's playback to a JACK client's inputs (or take it away),
    // port by port through pw-link. A user request, never automatic. The bool
    // reports acceptance; audioLinkBusy/message report asynchronous completion.
    Q_INVOKABLE bool linkOutputTo(int nodeId);
    Q_INVOKABLE bool unlinkOutputFrom(int nodeId);
    // Where OmaAudio keeps its files, for Preferences: one XDG home per role.
    Q_INVOKABLE QString settingsFilePath() const;
    Q_INVOKABLE QString stateDirectoryPath() const;
    Q_INVOKABLE QString cacheDirectoryPath() const;
    Q_INVOKABLE QString recordingsDirectoryPath() const;
    // What the launch-time copy from the earlier omawave folder did, if anything.
    Q_INVOKABLE QString storageMigrationMessage() const;

    Q_INVOKABLE QVariantMap trackById(int id) const;
    Q_INVOKABLE QVariantMap trackSourceInfo(int track = -1) const;
    Q_INVOKABLE int trackIndexById(int id) const;

    Q_INVOKABLE void open(const QString &path);      // replaces the project
    Q_INVOKABLE void openFile(const QString &path) { open(path); }
    Q_INVOKABLE void importFile(const QString &path);// adds a track
    // A queued UI import keeps its path until the worker accepts it.
    Q_INVOKABLE bool tryImportFile(const QString &path, bool openWhenEmpty = false);
    Q_INVOKABLE void importSession(const QString &path, const QString &mediaFolder = QString());
    Q_PROPERTY(QString sessionImportMessage READ sessionImportMessage NOTIFY sessionImportChanged)
    QString sessionImportMessage() const { return m_sessionImportMessage; }
    // Stops a file that is still being read or decoded; nothing is installed.
    Q_INVOKABLE void cancelLoad();
    // Which compressed containers this box can import: {aac: bool, reason: string}.
    Q_INVOKABLE QVariantMap importSupport() const;

    [[nodiscard]] bool recording() const { return m_recording != nullptr; }
    [[nodiscard]] QString recordingStatus() const { return m_recordingStatus; }
    [[nodiscard]] qint64 recordingFrames() const;
    [[nodiscard]] qint64 recordingStart() const { return m_recordPlacement.start; }
    [[nodiscard]] double inputPeak() const { return m_recording ? m_recording->peak() : 0; }
    [[nodiscard]] int recordArmedTrack() const { return m_recordArmedTrack; }
    [[nodiscard]] QString recordInput() const { return m_recordInput; }
    [[nodiscard]] int recordChannels() const { return m_recordChannels; }
    [[nodiscard]] bool recordMonitor() const { return m_recordMonitor; }
    [[nodiscard]] bool recordPunch() const { return m_recordPunch; }
    [[nodiscard]] double recordPreRoll() const { return m_recordPreRoll; }
    [[nodiscard]] int recordCountIn() const { return m_recordCountIn; }
    [[nodiscard]] int recordLoopTakes() const { return m_recordLoopTakes; }
    Q_INVOKABLE void setRecordLoopTakes(int count);
    Q_INVOKABLE QVariantMap routingTarget(int track = -1) const;
    Q_INVOKABLE bool setTrackRouting(int track, int output, int sidechain,
        const QVariantList& sends, const QString& stamp = {});
    [[nodiscard]] QVariantList additionalRecordInputs() const { return m_additionalRecordInputs; }
    Q_INVOKABLE void addRecordInput(const QString& input, int channels = 1, int track = -1);
    Q_INVOKABLE void removeRecordInput(int index);
    [[nodiscard]] double recordTempo() const { return m_recordTempo; }
    [[nodiscard]] bool recordAlign() const { return m_recordAlign; }
    [[nodiscard]] double recordOffsetMs() const { return m_recordOffsetMs; }
    [[nodiscard]] qint64 recordAlignmentFrames() const { return m_recordPlacement.alignment; }
    Q_INVOKABLE void setRecordingLeadIn(double preRollSeconds, int beats, double tempo);
    Q_INVOKABLE void setRecordingAlignment(bool automatic, double residualMs);
    [[nodiscard]] QStringList recordingInputNames() const { return m_recordingInputNames; }
    [[nodiscard]] QStringList recordingInputIds() const { return m_recordingInputIds; }
    [[nodiscard]] bool scanningInputs() const { return m_inputScan != nullptr; }
    [[nodiscard]] QVariantList recoverableRecordings() const { return m_recoverableRecordings; }
    Q_INVOKABLE void setRecordArmedTrack(int track);
    Q_INVOKABLE void setRecordInput(const QString& input);
    Q_INVOKABLE void setRecordChannels(int channels);
    Q_INVOKABLE void setRecordMonitor(bool enabled);
    Q_INVOKABLE void setRecordPunch(bool enabled);
    Q_INVOKABLE void refreshRecordingInputs();
    Q_INVOKABLE void cancelRecordingInputScan();
    Q_INVOKABLE void recoverRecording(const QString& path);
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

    [[nodiscard]] bool loopEnabled() const { return m_loopEnabled; }
    [[nodiscard]] qint64 loopStart() const { return m_loopStart; }
    [[nodiscard]] qint64 loopEnd() const { return m_loopEnd; }
    Q_INVOKABLE void toggleLoop();
    Q_INVOKABLE void setLoopEnabled(bool enabled);
    [[nodiscard]] bool clipMoving() const { return m_clipMove.has_value(); }
    [[nodiscard]] QVariantMap clipMovePreview() const { return m_clipMovePreview; }
    Q_INVOKABLE bool beginClipMove(int track, qint64 frame);
    Q_INVOKABLE bool beginClipSlip(int track, qint64 frame);
    Q_INVOKABLE void previewClipSlip(qint64 dragFrames);
    Q_INVOKABLE void slipFocusedClip(qint64 sourceDelta);
    Q_INVOKABLE void previewClipMove(int track, qint64 position, bool snap = true);
    Q_INVOKABLE void finishClipMove();
    Q_INVOKABLE void cancelClipMove();
    Q_INVOKABLE void play();
    // J/K/L: play at a whole multiple of normal speed, backwards when negative
    // (-8..8). Not while recording or writing automation.
    Q_INVOKABLE bool shuttle(int speed);
    [[nodiscard]] int playSpeed() const { return m_playing ? m_playSpeed : 1; }
    Q_INVOKABLE void stop();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekFrames(qint64 frame);
    Q_INVOKABLE void setGainDb(double db);           // master monitor level

    Q_INVOKABLE void setTrackGainDb(int track, double db);
    Q_INVOKABLE void setTrackPan(int track, double pan);
    Q_INVOKABLE void setTrackMute(int track, bool on);
    Q_INVOKABLE void setTrackSolo(int track, bool on);

    [[nodiscard]] qint64 selectionStart() const { return m_selection.start; }
    [[nodiscard]] qint64 selectionEnd() const { return m_selection.end; }
    [[nodiscard]] qint64 selectionLength() const { return m_selection.length(); }
    [[nodiscard]] bool selectionEmpty() const { return m_selection.empty(); }
    [[nodiscard]] int selectionTrack() const
    {
        return static_cast<int>(m_selection.track);
    }

    [[nodiscard]] bool dirty() const { return m_dirty; }
    [[nodiscard]] bool saving() const { return m_saving; }
    [[nodiscard]] bool saveQueued() const { return !m_saveRequests.empty(); }
    [[nodiscard]] QString projectPath() const { return m_projectPath; }
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] bool canUndo() const { return m_undo.canUndo(); }
    [[nodiscard]] bool canRedo() const { return m_undo.canRedo(); }
    [[nodiscard]] QString undoLabel() const;
    [[nodiscard]] bool anySolo() const { return m_project.anySoloed(); }

    [[nodiscard]] QVariantList plugins() const { return m_plugins; }
    [[nodiscard]] QVariantMap effectMenu() const { return m_effectMenu; }
    [[nodiscard]] bool pluginsListed() const { return m_pluginsListed; }
    [[nodiscard]] QStringList pluginScanWarnings() const { return m_pluginScanWarnings; }
    [[nodiscard]] bool scanning() const { return m_scanning; }
    [[nodiscard]] QVariantList pluginPorts() const { return m_pluginPorts; }
    [[nodiscard]] quint64 pluginValuesRevision() const { return m_pluginValuesRevision; }
    [[nodiscard]] quint64 pluginTextRevision() const { return m_pluginTextRevision; }
    Q_INVOKABLE QString pluginPortText(int slot, qint64 port, int track, double value);
    Q_INVOKABLE double pluginPortValue(int slot, qint64 port, int track, double fallback);

    // --- frequency response (backend_eq_curve.cpp) --------------------------------
    // What sort of effect a slot holds, from its class: "eq" (equalisers and
    // filters), "dynamics" (compressors, limiters, gates, expanders) or "other".
    Q_INVOKABLE QString pluginKind(int slot, int target = -1);
    // The slot's frequency response as it is set now, measured by playing an
    // impulse through a separate offline copy of the effect: {valid, pending,
    // reason, kind, bypassed, frequencies: [256 Hz values, 20 Hz–20 kHz, log
    // spaced], db: [the gain at each], bands: [{label, key, freqPort, gainPort,
    // qPort, freqMin, freqMax, gainMin, gainMax, qMin, qMax, freq, gain, q, qLog,
    // gainLinear (the gain is a factor, 1 = 0 dB, not dB), enablePort, enabled
    // (the band's own on/off switch, when it has one)}]} (ports -1 when the band
    // has none). The measuring runs in the background: the first
    // call for new settings returns the last result with `pending` and
    // pluginResponseRevision moves when the new one is in. Bands come from the
    // control names and units; a layout that is not recognised has none.
    Q_INVOKABLE QVariantMap pluginResponse(int slot, int target = -1);
    [[nodiscard]] quint64 pluginResponseRevision() const { return m_pluginResponseRevision; }
    [[nodiscard]] int pluginPortsSlot() const { return m_pluginPortsSlot; }
    [[nodiscard]] bool pluginPortsComplete() const { return m_pluginPortsComplete; }
    [[nodiscard]] QVariantList pluginOutputs() const { return m_pluginOutputs; }
    [[nodiscard]] QString pluginPortsKind() const { return m_pluginPortsKind; }
    [[nodiscard]] QString pluginPortsUri() const { return m_pluginPortsUri; }
    // A meter's value: what the running effect last wrote to an output control.
    // Lock-free; 0 when the slot is not running. Polled by the panel while it plays.
    Q_INVOKABLE double pluginOutputValue(int slot, qint64 port, int track = -1);
    // "dynamics" | "eq" | "other" for an installed effect's URI.
    Q_INVOKABLE QString pluginKind(const QString &uri);
    // Whether the audio output is open, so playback can actually be heard. A panel
    // that starts playing on its own checks this first.
    Q_INVOKABLE bool audioOutputRunning() const { return m_output.running(); }
    // Whether a QML file is part of this build (the EQ curve is optional).
    Q_INVOKABLE bool qmlAvailable(const QString &name) const { return QFile::exists(QStringLiteral(":/") + name); }

    // --- selection -----------------------------------------------------------------

    Q_INVOKABLE void select(qint64 start, qint64 end, bool snap = true, int track = -1);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    [[nodiscard]] QVariantList selectedTracks() const;
    Q_INVOKABLE bool setSelectedTracks(const QVariantList& tracks);
    Q_INVOKABLE void selectTrack(int track, bool toggle = false, bool extend = false);
    Q_INVOKABLE void focusSelectedTrack(int track);
    Q_INVOKABLE void clearGroupSelection();
    [[nodiscard]] QVariantList selectedClips() const;
    Q_INVOKABLE bool setSelectedClips(const QVariantList& clips, const QString& stamp = QString());
    Q_INVOKABLE bool selectClipAt(int track, qint64 frame, bool toggle = false);
    Q_INVOKABLE bool moveSelectedClips(qint64 deltaFrames, int deltaTracks, const QString& stamp = QString());
    [[nodiscard]] QVariantList clipGroups() const;
    [[nodiscard]] bool clipGroupsLinked() const { return m_clipGroupsLinked; }
    Q_INVOKABLE void setClipGroupsLinked(bool linked);
    Q_INVOKABLE bool createClipGroup(const QString& name, const QString& colour = QString(), const QString& stamp = QString());
    Q_INVOKABLE bool updateClipGroup(int group, const QString& name, const QString& colour, const QString& stamp = QString());
    Q_INVOKABLE bool detachSelectedClips(const QString& stamp = QString());
    Q_INVOKABLE bool dissolveClipGroup(int group, const QString& stamp = QString());

    // --- tracks --------------------------------------------------------------------

    Q_INVOKABLE void addTrack();
    Q_INVOKABLE void removeTrack(int track);
    // The tracks picked by their headers, or the focused track: one Undo step.
    Q_INVOKABLE void removeSelectedTracks();
    Q_INVOKABLE void renameTrack(int track, const QString &name);
    Q_INVOKABLE void moveTrack(int track, int toIndex);   // destination, clamped
    Q_INVOKABLE void focusTrack(int track);
    Q_INVOKABLE void focusNextTrack(int delta);
    Q_INVOKABLE void toggleMute(int track);
    Q_INVOKABLE void toggleSolo(int track);

    // Role and colour are undo steps, like a rename. Height is not: it is how the
    // session is looked at, not what is in it -- but it is saved with the project.
    Q_INVOKABLE void setTrackRole(int track, const QString &role);
    Q_INVOKABLE void setTrackColor(int track, const QString &color);   // "" = role's
    Q_INVOKABLE void setTrackHeight(int track, const QString &height);
    Q_INVOKABLE void stepTrackHeight(int track, int delta);
    // A copy directly below the original: clips, inserts, parameters and all.
    Q_INVOKABLE void duplicateTrack(int track);

    // --- markers -------------------------------------------------------------------

    [[nodiscard]] QVariantList markers() const { return m_markers; }
    Q_INVOKABLE int addMarker(qint64 frame, const QString &name = QString(),
                              const QString &type = QStringLiteral("cue"));
    Q_INVOKABLE int addRangeMarker(qint64 start, qint64 end,
                                   const QString &name = QString());
    Q_INVOKABLE void removeMarker(int id);
    Q_INVOKABLE void renameMarker(int id, const QString &name);
    Q_INVOKABLE void moveMarker(int id, qint64 position, qint64 length = -1);
    Q_INVOKABLE void setMarkerType(int id, const QString &type);
    Q_INVOKABLE void setMarkerColor(int id, const QString &color);
    Q_INVOKABLE void setMarkerNotes(int id, const QString &notes);
    Q_INVOKABLE QVariantMap markerById(int id) const;
    // The nearest marker strictly after / before a frame, or -1.
    Q_INVOKABLE qint64 nextMarkerFrame(qint64 from) const;
    Q_INVOKABLE qint64 prevMarkerFrame(qint64 from) const;
    // Seeks to the next (+1) or previous (-1) marker from the playhead.
    Q_INVOKABLE void jumpToMarker(int direction);

    // --- snapping, ripple, history, monitor ----------------------------------------

    [[nodiscard]] bool snapping() const { return m_snapping; }
    void setSnapping(bool on);
    [[nodiscard]] qint64 snapToleranceFrames() const { return m_snapTolerance; }
    void setSnapToleranceFrames(qint64 frames);
    [[nodiscard]] QString snapGrid() const { return m_snapGrid; }
    void setSnapGrid(const QString& key);
    [[nodiscard]] QStringList snapGrids() const;
    Q_INVOKABLE QString snapGridLabel(const QString& key) const;
    [[nodiscard]] qint64 snapGridTicks() const;
    [[nodiscard]] bool musicalSnapActive() const { return m_snapping && snapGridTicks() > 0; }
    [[nodiscard]] QString snapHint() const;
    // What select() does to an edge: the nearest clip boundary, marker or playhead
    // within the tolerance, else the musical grid or nearest zero crossing. Identity when snapping
    // is off.
    Q_INVOKABLE qint64 snapFrame(qint64 frame) const;

    [[nodiscard]] bool ripple() const { return m_ripple; }
    void setRipple(bool on);

    [[nodiscard]] QVariantList history() const;
    [[nodiscard]] int historyCursor() const { return static_cast<int>(m_undo.cursor()); }
    // Undoes or redoes until `cursor` commands are applied.
    Q_INVOKABLE void jumpHistory(int cursor);

    [[nodiscard]] double masterGainDb() const { return m_masterGainDb; }
    [[nodiscard]] bool metronome() const { return m_metronome; }
    [[nodiscard]] bool checkingComputer() const { return m_checkingComputer; }
    [[nodiscard]] QVariantMap computerCheck() const { return m_computerCheck; }
    [[nodiscard]] bool aligningSpeech() const { return m_aligningSpeech; }
    [[nodiscard]] bool remixing() const { return m_remixing; }
    [[nodiscard]] QString remixMessage() const { return m_remixMessage; }
    [[nodiscard]] QString speechAlignMessage() const { return m_speechAlignMessage; }
    [[nodiscard]] double metronomeLevelDb() const { return m_metronomeLevelDb; }
    void setMetronome(bool on);
    void setMetronomeLevelDb(double db);

    [[nodiscard]] QVariantList trackMeters() const { return m_trackMeters; }
    [[nodiscard]] int meterRevision() const { return m_meterRevision; }
    Q_INVOKABLE QVariantMap trackMeter(int id) const;

    [[nodiscard]] QStringList trackRoles() const;
    [[nodiscard]] QStringList trackHeights() const;
    [[nodiscard]] QStringList markerTypes() const;

    // --- the master bus ---
    static constexpr int kMasterTarget = -2;
    [[nodiscard]] int masterTarget() const { return kMasterTarget; }
    [[nodiscard]] QVariantList masterInserts() const { return m_masterInserts; }
    [[nodiscard]] qint64 masterLatency() const { return m_masterLatency; }
    [[nodiscard]] double masterTrimDb() const;
    Q_INVOKABLE void setMasterTrimDb(double db);
    [[nodiscard]] int pluginPortsTarget() const { return m_pluginPortsTarget; }

    [[nodiscard]] QVariantMap loudness() const { return m_loudness; }
    Q_INVOKABLE void resetLoudness();
    [[nodiscard]] bool previewBefore() const { return m_previewBefore; }
    void setPreviewBefore(bool on);
    [[nodiscard]] bool hearWithoutEffects() const { return m_hearWithoutEffects; }
    void setHearWithoutEffects(bool on);
    [[nodiscard]] double previewMatchDb() const { return m_previewMatchDb; }
    // False until an analysis has measured both mixes; the preview is then a plain
    // bypass and the page says so.
    [[nodiscard]] bool previewMatched() const { return m_previewMatched; }

    [[nodiscard]] bool analysing() const { return m_analysing; }
    [[nodiscard]] bool analysed() const { return m_analysis.ok; }
    [[nodiscard]] bool analysisStale() const { return m_analysisStale; }
    [[nodiscard]] QVariantList masterLevels() const { return m_masterLevels; }
    [[nodiscard]] qint64 masterFrames() const { return m_analysis.frames; }
    [[nodiscard]] QVariantMap masterReport() const { return m_masterReport; }
    [[nodiscard]] QVariantList shortTermHistory() const { return m_shortTermHistory; }
    [[nodiscard]] QVariantList momentaryHistory() const { return m_momentaryHistory; }
    [[nodiscard]] int historyHopFrames() const { return static_cast<int>(m_analysis.hop); }
    // Renders the mastered mix on the worker and keeps its waveform, loudness history
    // and report. Refused while the worker is busy, like a save (trap 34).
    Q_INVOKABLE void analyseMaster();

    [[nodiscard]] bool measuringTrackLoudness() const { return m_measuringTrackLoudness; }
    [[nodiscard]] double trackLoudnessProgress() const { return m_trackLoudnessProgress; }
    [[nodiscard]] QVariantMap trackLoudnessReport() const;
    Q_INVOKABLE bool measureTrackLoudness(int track = -1);
    Q_INVOKABLE void cancelTrackLoudness();
    [[nodiscard]] bool generatingDucking() const { return m_generatingDucking; }
    [[nodiscard]] double duckingProgress() const { return m_duckingProgress; }
    [[nodiscard]] QVariantMap duckingPreview() const;
    Q_INVOKABLE bool analyseDucking(const QVariantMap& spec);
    Q_INVOKABLE void cancelDucking();
    // keepUpdated stores the recipe so the curve is generated again whenever a
    // trigger track's sound changes, until its points are edited by hand.
    Q_INVOKABLE bool applyGeneratedDucking(const QString& token, bool keepUpdated = false);
    Q_INVOKABLE QVariantMap generatedDuckingState(int track) const;
    Q_INVOKABLE bool stopFollowingDucking(int track);
    // Puts back the volume curve a generated ducking that still follows its
    // triggers was made from, and forgets the recipe (one Undo step).
    Q_INVOKABLE bool removeGeneratedDucking(int track);
    Q_INVOKABLE QVariantMap trackLoudnessProposal(int track, double targetLufs, double ceilingDbTp) const;
    Q_INVOKABLE bool applyTrackLoudness(int track, const QString &stamp, double targetLufs, double ceilingDbTp);
    Q_INVOKABLE QVariantMap essentialProcessor(const QString &module, int track = -1, int slot = -2);
    Q_INVOKABLE QVariantMap voiceCleanupTarget() const;
    Q_INVOKABLE bool applyVoiceCleanup(double amount, const QString &stamp);
    Q_INVOKABLE bool applyEssentialProcessor(const QString &module, int track, int slot,
        const QString &stamp, bool enabled, const QVariantMap &values, int sidechain = -1);
    // The Sound panel's voice fixes (backend_sound.cpp): read them, and change any
    // of them as one Undo step. `changes` maps a fix id to {on, amount, mains}.
    Q_INVOKABLE QVariantMap soundSettings(int track = -1);
    Q_INVOKABLE bool applySound(int track, const QString &stamp, const QVariantMap &changes);
    // The Sound panel's choices that are not effects (loudness target, room echo
    // amount), kept per track with the project's view: saved, never an undo step.
    Q_INVOKABLE QVariantMap soundChoices(int track) const;
    Q_INVOKABLE void setSoundChoice(int track, const QString &key, double value);

    [[nodiscard]] QVariantList exportFormats() const;
    [[nodiscard]] QVariantList deliveryPresets() const;
    Q_INVOKABLE QUrl suggestedExportUrl(const QString &format) const;
    [[nodiscard]] DisplayCache displayCache() const { return m_displayCache; }
    Q_INVOKABLE QVariantMap presetById(const QString &id) const;
    [[nodiscard]] QVariantList renderQueue() const;
    [[nodiscard]] bool rendering() const { return m_activeRender >= 0; }
    [[nodiscard]] double renderProgress() const { return m_renderProgress; }
    // Queues a delivery described by a map (path, format, bitDepth, sampleRate,
    // channels, dither, normalise, targetLufs, ceilingDbTp, peakTargetDb, name) and
    // starts it when the worker is free. Returns the job id, or -1 with lastError.
    Q_INVOKABLE int queueRender(const QVariantMap &spec);
    [[nodiscard]] QVariantList fileBatchJobs() const;
    [[nodiscard]] bool fileBatchBusy() const;
    [[nodiscard]] QVariantMap fileBatchStatus() const;
    Q_INVOKABLE QVariantMap previewFileBatch(const QStringList& paths, const QVariantMap& settings) const;
    Q_INVOKABLE int queueFileBatch(const QStringList& paths, const QVariantMap& settings);
    Q_INVOKABLE void cancelBatchFile(int id);
    Q_INVOKABLE void cancelFileBatch(int batch = -1);
    Q_INVOKABLE bool retryBatchFile(int id);
    Q_INVOKABLE bool resumeBatchFile(int id);
    Q_INVOKABLE bool resumeFileBatch();
    Q_INVOKABLE void clearFinishedBatchFiles();
    Q_INVOKABLE bool resetFileBatchQueue();
    Q_INVOKABLE void cancelRender(int id);
    Q_INVOKABLE void retryRender(int id);
    Q_INVOKABLE void removeRender(int id);
    Q_INVOKABLE void clearFinishedRenders();
    Q_INVOKABLE void revealRender(int id);
    Q_INVOKABLE QVariantMap renderById(int id) const;

    // --- the Spectral page ---------------------------------------------------------

    [[nodiscard]] QVariantMap spectral() const { return m_spectral; }
    [[nodiscard]] bool spectralBuilding() const { return m_spectralBuilding; }
    [[nodiscard]] bool spectralStale() const { return m_spectralStale; }
    [[nodiscard]] bool spectralWanted() const { return m_spectralWanted; }
    // The page sets this while it is up. Nothing is analysed for a page nobody is
    // looking at: a spectrum is a full pass over the take.
    void setSpectralWanted(bool wanted);
    [[nodiscard]] double spectralProgress() const { return m_spectralProgress; }
    [[nodiscard]] int spectralFftSize() const { return static_cast<int>(m_stft.fftSize); }
    [[nodiscard]] QString spectralWindow() const;
    // The overlap as a divisor: hop = fftSize / overlap. 2, 4 or 8 (50 %, 75 %, 87.5 %).
    [[nodiscard]] int spectralOverlap() const { return m_overlap; }
    [[nodiscard]] QVariantList spectralFftSizes() const;
    [[nodiscard]] QStringList spectralWindows() const;
    [[nodiscard]] QVariantList spectralOverlaps() const;
    // A change rebuilds the display (if the page is up) and is what the repairs use.
    Q_INVOKABLE void setSpectralConfig(int fftSize, const QString &window, int overlap);
    Q_INVOKABLE void rebuildSpectral();

    // The frequency band of the selection. Empty (0, 0) means the whole spectrum,
    // which is what a plain time selection is. High of 0 means Nyquist.
    [[nodiscard]] double bandLowHz() const { return m_bandLowHz; }
    [[nodiscard]] double bandHighHz() const { return m_bandHighHz; }
    [[nodiscard]] bool bandEmpty() const { return m_bandLowHz <= 0.0 && m_bandHighHz <= 0.0; }
    Q_INVOKABLE void setBand(double lowHz, double highHz);
    Q_INVOKABLE void clearBand();

    // The mask: shapes drawn on the spectrogram in seconds and Hz, session state
    // like the band. While it holds anything the repairs take it instead of the
    // band (inside the time selection, which every added shape widens to hold
    // it). A lasso is a closed list of {seconds, hz}; a stroke is a list of
    // {seconds, hz, rSeconds, hzLow, hzHigh} dabs. `subtract` cuts instead of adds.
    [[nodiscard]] QVariantList maskShapes() const { return m_maskShapes; }
    [[nodiscard]] bool maskEmpty() const { return m_mask.empty(); }
    [[nodiscard]] int maskShapeCount() const { return static_cast<int>(m_mask.shapes.size()); }
    [[nodiscard]] int maskFeather() const { return m_maskFeather; }
    Q_PROPERTY(bool pitchTracking READ pitchTracking NOTIFY maskChanged)
    [[nodiscard]] bool pitchTracking() const { return m_pitchTracking; }
    Q_INVOKABLE void cancelPitchTracking();
    void setMaskFeather(int cells);
    // `subtract` cuts; `intersect` keeps only what the mask already had inside
    // the shape (A25). Neither is an add.
    Q_INVOKABLE void addMaskLasso(const QVariantList &points, bool subtract = false, bool intersect = false);
    Q_INVOKABLE void addMaskStroke(const QVariantList &stamps, bool subtract = false, bool intersect = false);
    Q_INVOKABLE void undoMaskShape();
    Q_INVOKABLE void clearMask();
    // A harmonic series as one shape (A24): `partials` bands of `width` (cents, or
    // Hz when `widthInHz`) round every multiple of `hz`, from `fromSeconds` to
    // `toSeconds` -- or, with no span (to <= from), over the time selection, or the
    // whole take. `snap` refines the pick to the partial the spectrum has beside it
    // at the picked time (the span's middle by default). `followPitch` traces
    // that partial on the worker; `includeFundamental` off is the overtones alone.
    Q_INVOKABLE void addMaskHarmonics(double fromSeconds, double toSeconds, double hz, int partials,
                                      double width, bool widthInHz, bool includeFundamental,
                                      bool subtract = false, bool snap = true, bool intersect = false,
                                      bool followPitch = false, double seedSeconds = -1);
    // The partial the spectrum has nearest `hz` at `seconds` on the focused take,
    // or `hz` itself when nothing there stands up.
    Q_INVOKABLE double partialNear(double seconds, double hz);
    // Heal under one stroke straight away, mask and selection untouched: the spot
    // tool. The region is the stroke's own extent plus the feather.
    Q_INVOKABLE void spotHeal(const QVariantList &stamps);

    // The repairs: the selection's band, inside the selection's time, on the focused
    // take. Rendered on the worker, then the result becomes a new source laid over
    // the range -- one undo step, like any edit. Refused while the worker is busy.
    // A repair asked for while the worker slot is busy waits for it rather than
    // being refused: `repairing` is true from the ask, `repairQueued` until it runs.
    [[nodiscard]] bool repairing() const { return m_repairing; }
    [[nodiscard]] bool repairQueued() const { return m_queuedRepair.has_value(); }
    Q_INVOKABLE void attenuateSpectral(double gainDb);
    Q_INVOKABLE void healSpectral();
    Q_INVOKABLE void declipSpectral(double thresholdDb = -1.0);
    Q_INVOKABLE void dereverbSpectral(double amountDb = 12, double decaySeconds = 0.6, double strength = 1);
    // The Sound tab's Room echo: late-reverb reduction written into the focused
    // track's audio, over the selection on it or else the whole track, on every
    // channel and frequency. `amount` is 0..1. One Undo step when it lands.
    Q_INVOKABLE bool reduceRoomEcho(int track, double amount);
    Q_INVOKABLE void cancelRepair();
    [[nodiscard]] QString repairSummary() const { return m_repairSummary; }
    [[nodiscard]] QString repairChannel() const { return m_repairChannel; }
    void setRepairChannel(const QString&);
    [[nodiscard]] QString spectralView() const { return m_spectralView; }
    void setSpectralView(const QString&);
    // The noise print: the mean spectrum of the selection, kept for the session
    // (it is a measurement of this take, not a setting). DeNoise subtracts it inside
    // the selection's band, `strength` times over, never by more than `amountDb`.
    [[nodiscard]] QVariantList clicks() const { return m_clickList; }
    [[nodiscard]] int clickCount() const { return static_cast<int>(m_clicks.size()); }
    [[nodiscard]] bool detecting() const { return m_detecting; }
    [[nodiscard]] QString clickSummary() const { return m_clickSummary; }
    // Runs the detector over the selection, or the whole take without one, on the
    // worker; the list lands through clicksChanged. `sensitivity` 0..1.
    Q_INVOKABLE void detectClicks(double sensitivity);
    Q_INVOKABLE void clearClicks();
    // One repair, "heal clicks", over every click found: a new source from the
    // first to the last with each event interpolated across in the time domain.
    Q_INVOKABLE void healClicks();
    // One click from the list, on its own: "heal click", and the others stay
    // listed (a repair that lays audio over a range moves none of them).
    Q_INVOKABLE void healClickAt(int index);
    // Detect, then heal what was found, as one gesture (Find and Repair Clicks).
    Q_INVOKABLE void autoHealClicks(double sensitivity);
    [[nodiscard]] bool noisePrintBuilding() const { return m_noisePrintActive || m_noisePrintRequest.has_value(); }
    [[nodiscard]] bool noisePrintReady() const { return !m_noisePrint.empty(); }
    [[nodiscard]] QString noisePrintSummary() const { return m_noisePrintSummary; }
    Q_INVOKABLE void captureNoisePrint();
    Q_INVOKABLE void clearNoisePrint();
    Q_INVOKABLE void denoiseSpectral(double amountDb, double strength);

    // --- editing -------------------------------------------------------------------

    Q_INVOKABLE void split(qint64 frame);
    Q_INVOKABLE void deleteSelection();      // leaves the gap (A13)
    Q_INVOKABLE void deleteClosingGap();     // removes and ripples what follows earlier
    Q_INVOKABLE void deleteTimeAllTracks();  // one selected interval, every track
    Q_INVOKABLE void cut();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste(qint64 frame);
    Q_INVOKABLE void changeProjectSamples(int rate, int exportBits, const QString& stamp = QString());
    [[nodiscard]] int defaultExportBits() const { return int(m_project.defaultExportBits()); }
    [[nodiscard]] bool storageBusy() const { return m_storageActive || m_storageRequest.has_value(); }
    [[nodiscard]] QString storageMessage() const { return m_storageMessage; }
    [[nodiscard]] QVariantList unusedAudioFiles() const { return m_unusedAudioFiles; }
    [[nodiscard]] QString storagePreviewStamp() const { return m_storagePreviewStamp; }
    [[nodiscard]] bool elasticPreparing() const;
    [[nodiscard]] bool elasticFailed() const;
    [[nodiscard]] qulonglong streamCacheMisses() const;
    [[nodiscard]] qulonglong streamReadErrors() const;
    [[nodiscard]] int systemMemoryMiB() const;
    [[nodiscard]] int maximumMemoryMiB() const;
    [[nodiscard]] int memoryMiB() const { return m_memoryMiB; }
    [[nodiscard]] int effectiveMemoryMiB() const;
    [[nodiscard]] int playbackCacheMiB() const;
    void setMemoryMiB(int mib);
    [[nodiscard]] QString timeDisplay() const { return m_timeDisplay; }
    void setTimeDisplay(const QString &key);
    [[nodiscard]] QStringList timeDisplays() const;
    [[nodiscard]] QString timeDisplayHint() const;
    [[nodiscard]] qint64 timeStep() const;
    Q_INVOKABLE QString timeDisplayLabel(const QString &key) const;
    // A position in project samples, as the chosen display writes it,
    // and what a person typed back as samples (-1 when it is not a time).
    Q_INVOKABLE QString formatTime(qint64 samples) const;
    Q_INVOKABLE QString formatDuration(qint64 samples) const;
    Q_INVOKABLE qint64 parseDuration(const QString &text) const;
    [[nodiscard]] QString durationDisplayHint() const;
    [[nodiscard]] QVariantMap musicalTime() const;
    Q_INVOKABLE bool setMusicalTime(bool enabled, double tempo, int beats, int unit, const QString &stamp = QString());
    Q_INVOKABLE qint64 musicalTickAt(qint64 samples) const;
    Q_INVOKABLE qint64 sampleAtMusicalTick(qint64 tick) const;
    Q_INVOKABLE qint64 parseTime(const QString &text) const;
    Q_INVOKABLE qint64 advanceTime(qint64 samples, int steps) const;
    Q_INVOKABLE QString formatShortLength(qint64 samples) const;
    Q_INVOKABLE QString formatPerPixel(double samplesPerPixel) const;
    [[nodiscard]] int spectralStoreGiB() const { return m_spectralStoreGiB; }
    void setSpectralStoreGiB(int gib);
    [[nodiscard]] QString spectralStoreUsage() const;
    [[nodiscard]] QString spectralStoreStatus() const { return m_spectralStoreStatus; }
    [[nodiscard]] bool spectralStoreBusy() const { return m_spectralStoreBusy; }
    Q_INVOKABLE void refreshSpectralStore();
    Q_INVOKABLE void clearSpectralStore();
    // How many spectral displays this session took from the store, and how long
    // a build must take before its tiles are worth keeping. For tests and tuning.
    // What the running plugin instance holds for a control, as opposed to what
    // the project says. For tests; read with the transport idle.
    [[nodiscard]] double livePluginControl(int slot, qint64 port, int track = -1);
    // How many times a plugin chain has actually been rebuilt. For tests.
    [[nodiscard]] quint64 chainRebuilds() const { return m_chainRebuilds; }
    [[nodiscard]] quint64 spectralStoreHits() const { return m_spectralStoreHits.load(std::memory_order_relaxed); }
    void setSpectralStoreMinimumMs(int ms) { m_spectralStoreMinimumMs.store(ms, std::memory_order_relaxed); }
    Q_INVOKABLE QVariantMap storageUsage() const;
    Q_INVOKABLE void pruneUnusedSources(const QString& stamp = QString());
    Q_INVOKABLE void inspectUnusedAudio();
    Q_INVOKABLE void removeUnusedAudio(const QString& stamp);
    Q_INVOKABLE void cancelStorage();
    Q_INVOKABLE void mixPaste(qint64 frame = -1, double incomingDb = 0, double destinationDb = 0);
    Q_INVOKABLE void insertSilence(qint64 frames, bool allTracks = false, qint64 at = -1);
    Q_INVOKABLE void mapTrackChannels(const QVariantList& matrix, const QString& stamp = QString());
    [[nodiscard]] int selectionChannel() const { return m_selection.channel; }
    void setSelectionChannel(int channel);
    [[nodiscard]] QString clipboardSummary() const;
    Q_INVOKABLE void trim();
    Q_INVOKABLE void gainSelection(double db);
    // Silence is delete-leaving-the-gap whatever the ripple switch says (Phase 2).
    Q_INVOKABLE void silenceSelection();
    // Gains the selection so its loudest sample lands at `targetDb` (a peak
    // normalise, through the clip gain -- non-destructive like every other edit).
    [[nodiscard]] bool sampleEditing() const { return m_groupStretchDeferred || m_sampleEditActive || m_sampleEditRequest.has_value(); }
    [[nodiscard]] bool sampleEditQueued() const { return m_groupStretchDeferred || m_sampleEditRequest.has_value(); }
    [[nodiscard]] QString sampleEditLabel() const { return m_sampleEditLabel; }
    Q_INVOKABLE void reverseSelection();
    Q_INVOKABLE QVariantMap stretchTarget() const;
    Q_INVOKABLE QVariantMap clipProperties() const;
    Q_INVOKABLE QVariantMap clipEnvelopeTarget(int track = -1, int clip = -1) const;
    // Clip effects (backend_clip_effects.cpp). A clip's rack is shared by its
    // split views; edits replace it for all of them. Sound is rendered off the
    // callback; until it is ready the clip is silent, never played dry.
    Q_INVOKABLE QVariantMap clipEffectsTarget(int track = -1, int clip = -1);
    Q_INVOKABLE bool addClipEffect(int track, int clip, const QString& uri, const QString& stamp);
    Q_INVOKABLE bool addEffectToSelectedClips(const QString& uri, const QString& stamp);
    Q_INVOKABLE bool removeClipEffect(int track, int clip, int slot, const QString& stamp);
    Q_INVOKABLE bool moveClipEffect(int track, int clip, int from, int to, const QString& stamp);
    Q_INVOKABLE bool setClipEffectBypass(int track, int clip, int slot, bool bypass, const QString& stamp);
    Q_INVOKABLE bool setClipEffectControl(int track, int clip, int slot, qint64 port, double value, const QString& stamp);
    Q_INVOKABLE bool editClipEffectAutomation(int track, int clip, int slot, qint64 port, const QString& operation,
                                             qint64 frame, qint64 to, double value, const QString& stamp);
    Q_INVOKABLE bool clearClipEffects(int track, int clip, const QString& stamp);
    Q_INVOKABLE bool bakeClipEffects(int track, int clip, const QString& stamp);
    Q_PROPERTY(int clipEffectsRendering READ clipEffectsRendering NOTIFY clipEffectsChanged)
    Q_PROPERTY(QString clipEffectsProblem READ clipEffectsProblem NOTIFY clipEffectsChanged)
    [[nodiscard]] int clipEffectsRendering() const { return int(m_clipEffectJobs.size()); }
    [[nodiscard]] QString clipEffectsProblem() const;
    Q_INVOKABLE bool editClipEnvelope(int track, int clip, const QString& key, const QString& operation,
                                     qint64 frame, qint64 to, double value, const QString& stamp);
    Q_INVOKABLE bool applyClipProperties(int track, int clip, const QString &stamp,
                                        double gainDb, bool replaceFades,
                                        qint64 fadeIn, qint64 fadeOut, const QVariantMap& fadeShapes = QVariantMap());
    // One clip's fade lengths, as the lane's fade handles set them: one Undo
    // step, shapes kept, the gain untouched.
    Q_INVOKABLE bool setClipFades(int track, int clip, qint64 fadeIn, qint64 fadeOut);
    // One clip's gain in dB, as the lane's gain line sets it: one Undo step,
    // fades untouched.
    Q_INVOKABLE bool setClipGainDb(int track, int clip, double gainDb);
    // How far a clip's edges can be dragged out: {valid, earliestStart,
    // latestEnd, minimumLength} in frames. The source, a clip effect's prepared
    // audio, a stored envelope and the neighbouring clips set the limits.
    Q_INVOKABLE QVariantMap clipTrimLimits(int track, int clip) const;
    // Moves a clip's edges to [start, end) over its source, the audio staying
    // where it is in time: one Undo step. Never the file, never other clips.
    Q_INVOKABLE bool trimClip(int track, int clip, qint64 start, qint64 end);
    Q_INVOKABLE bool applySelectedClipProperties(const QString& stamp, double gainDb, bool relativeGain,
                                                 bool replaceFades, qint64 fadeIn, qint64 fadeOut,
                                                 bool proportionalFades, const QVariantMap& fadeShapes = QVariantMap());
    Q_INVOKABLE void setFocusedClipElastic(qint64 frames, double semitones, const QString& stamp, int track, int clip);
    Q_INVOKABLE QVariantMap selectedStretchTarget() const;
    Q_INVOKABLE bool stretchSelectedClips(qint64 targetFrames, bool realtime, double pitchChange, const QString& stamp);
    Q_INVOKABLE void stretchFocusedClip(qint64 targetFrames, const QString& stamp, int track, int clip);
    Q_INVOKABLE void crossfadeSelection(bool equalPower = false);
    Q_INVOKABLE QVariantMap crossfadeTarget(int track = -1, int clip = -1) const;
    Q_INVOKABLE void adjustCrossfade(qint64 start, qint64 end, bool equalPower,
                                     const QString& stamp, int track, int clip);
    Q_INVOKABLE void convertTrackChannels(int channels);
    Q_INVOKABLE void cancelSampleEdit();
    // Apply an effect to the selection (backend_effect_apply.cpp). Begin puts the
    // effect first on the focused track's chain so it can be heard and adjusted;
    // Apply writes it into the selection's audio as one Undo step, and Cancel
    // takes it away. Either way the preview leaves no trace in the history.
    Q_INVOKABLE bool beginEffectPreview(const QString& uri);
    Q_INVOKABLE bool applyEffectPreview();
    // Applies the effect last applied to a selection again, with the same
    // settings, to the selection now on the focused track: one Undo step.
    Q_INVOKABLE bool repeatLastEffect();
    // A small picture of a stretch of a track: the loudest sample in each of
    // `buckets` equal parts of [start, end), 0..1, read from the clips before any
    // effect. Empty for a range too long to read quickly (over 20 minutes).
    Q_INVOKABLE QVariantList selectionPeaks(int track, qint64 start, qint64 end, int buckets) const;

    // --- tidy pauses (backend_pauses.cpp) ------------------------------------------
    // Finds the pauses between words. spec: {scope: "selection" | "track" |
    // "voices" (every voice track, the whole project), auto: bool, thresholdDb
    // (used when auto is false), minSeconds, paddingSeconds, keepBreaths}. A pause is
    // where every track looked at is quieter than the level for at least minSeconds;
    // with keepBreaths a breath in a gap is not part of it. Nothing changes. The
    // result is also `pauses`: {valid, stale, scope, tracks: [ids], trackNames, start,
    // end, thresholdDb, noiseFloorDb, auto, minSeconds, paddingSeconds, keepBreaths,
    // count, seconds, kept, pauses: [{index, start, end, seconds, kept}]} — frames
    // for start/end; `stale` once the project has changed since.
    Q_INVOKABLE QVariantMap findPauses(const QVariantMap& spec);
    // Keep one pause as it is (click a shaded pause), or let it be changed again.
    Q_INVOKABLE bool setPauseKept(int index, bool kept);
    Q_INVOKABLE void clearPauses();
    // What an action would do, without doing it. action: {then: "shorten" |
    // "remove" | "mark" | "split", shortenTo (s), moveOthers (bool: effects,
    // ambience and other non-music tracks move too; music never does), smoothMs,
    // markAs: "marker" (default) | "pause" | "speech"} → {valid, reason, changes
    // (how many pauses would change), changed: [their indices], lengths: [each one's
    // seconds after, in the same order], lengthBefore, lengthAfter, removedSeconds,
    // joins: [frames where joins would be, before the change]}.
    Q_INVOKABLE QVariantMap pausePlan(const QVariantMap& action) const;
    // Does it: one Undo step ("shorten 35 pauses", "remove 35 pauses", "mark 38
    // pauses", "split at 38 pauses"). The pauses are cleared and the result is
    // `pauseResult`: {then, changed, kept, total, lengthBefore, lengthAfter,
    // removedSeconds, smoothMs, moved (track count), joins: [frames, after the
    // change], undoLabel}. False with lastError when the pauses are stale.
    Q_INVOKABLE bool applyPauses(const QVariantMap& action);
    [[nodiscard]] QVariantMap pauses() const;
    [[nodiscard]] QVariantMap pauseResult() const { return m_pauseResult; }
    Q_INVOKABLE void clearPauseResult();

    // --- recipes (backend_recipes.cpp) ----------------------------------------------
    // A recipe is a named list of steps run in order, written into the audio, as
    // one Undo step. A step is a map: {kind, on, ...its settings}; see
    // recipeStepKinds() for the kinds, their settings and ranges. Built-in recipes
    // (builtIn: true) cannot be changed; saving one stores a copy. Own recipes are
    // kept on this computer for every project.
    [[nodiscard]] QVariantList recipes() const;            // {id, name, builtIn, steps, summary}
    Q_INVOKABLE QVariantMap recipe(const QString& id) const;
    // The kinds a step can be, in picker groups: {kind, group, groupTitle, title,
    // settings: [{key, label, from, to, step, unit, default, choices?}], available,
    // reason}. "effect" steps (any installed effect) are made with
    // effectStep(uri) instead.
    Q_INVOKABLE QVariantList recipeStepKinds();
    // A step for an installed effect at its default settings, or {} with lastError.
    Q_INVOKABLE QVariantMap effectStep(const QString& uri);
    // The settings a step can be changed by, as recipeStepKinds() lists them; for
    // an installed effect, its controls (keys "c:<symbol>", values the effect's own).
    Q_INVOKABLE QVariantList stepSettings(const QVariantMap& step);
    // A step's title and one-line summary of its settings ("Below 80 Hz").
    Q_INVOKABLE QString stepTitle(const QVariantMap& step) const;
    Q_INVOKABLE QString stepSummary(const QVariantMap& step) const;
    // Saves an own recipe ({id?, name, steps}); a missing or built-in id makes a
    // new one. Returns its id, or "" with lastError.
    Q_INVOKABLE QString saveRecipe(const QVariantMap& recipe);
    Q_INVOKABLE QString duplicateRecipe(const QString& id);
    Q_INVOKABLE bool renameRecipe(const QString& id, const QString& name);
    Q_INVOKABLE bool removeRecipe(const QString& id);
    // What a run on "selection" or "track" would cover: {valid, text, reason}.
    Q_INVOKABLE QVariantMap recipeTarget(const QString& target) const;
    // Runs a saved recipe on the selection or the whole focused track. The run
    // reports through recipeRun: {active, running, finished, stopped, id, name,
    // target, targetText, current (1-based, counting only steps that are on),
    // total (steps that are on), done, failed, seconds, undoLabel,
    // steps: [{title, summary, state: waiting|running|done|failed|off, seconds,
    // result, reason, progress}]}. A step that cannot run is reported and the
    // rest carry on.
    Q_INVOKABLE bool runRecipe(const QString& id, const QString& target);
    [[nodiscard]] QVariantMap recipeRun() const { return m_recipeRunView; }
    [[nodiscard]] bool recipeRunning() const { return m_recipeRunner.has_value(); }
    // Stops the run in progress and puts the audio back as it was.
    Q_INVOKABLE void stopRecipe();
    // Dismisses a finished or stopped run's report.
    Q_INVOKABLE void clearRecipeRun();
    Q_INVOKABLE bool runLastRecipeAgain();
    [[nodiscard]] QString lastRecipeName() const;
    // Record steps: the effects, Voice Cleanup, Room echo, click repair and fades
    // applied while it is on become steps, with their settings.
    [[nodiscard]] bool recordingSteps() const { return m_recordingSteps; }
    [[nodiscard]] QVariantList recordedSteps() const { return m_recordedSteps; }
    Q_INVOKABLE void startRecordingSteps();
    // Stops recording; returns the steps (and forgets them unless keep is true,
    // in which case they stay in recordedSteps until the next start).
    Q_INVOKABLE QVariantList stopRecordingSteps(bool keep = true);
    [[nodiscard]] QString lastEffectName() const { return m_lastEffect ? QString::fromStdString(m_lastEffect->name) : QString(); }
    Q_INVOKABLE void cancelEffectPreview();
    [[nodiscard]] QVariantMap effectPreview() const;
    Q_INVOKABLE void normalizeSelection(double targetDb = -1.0);
    // Analysis panels (backend_tools.cpp): kind "statistics", "spectrum" or
    // "phase" over the selection, or the focused track when nothing is selected.
    Q_INVOKABLE QVariantMap analyseAudio(const QString& kind, int fftSize = 4096, const QString& window = QStringLiteral("hann"));
    // Spectrum and phase of the newest programme output (the mix before the
    // monitor level), for the panel's live view while playing. `fresh` is false
    // when nothing new has played since the last call.
    Q_INVOKABLE QVariantMap liveAnalysis(int fftSize = 4096);
    // Audio load (backend_performance.cpp): share of each audio block playback uses
    // (percent, with a decaying peak), dropouts PipeWire reported, overruns, and the
    // effect chains costing most. `active` is false when nothing has played since
    // the previous call.
    Q_INVOKABLE QVariantMap audioLoad() const;
    // Measures mixing on this computer at the current rate and buffer size (worker,
    // a few seconds) and reports how many tracks of each kind fit in half of each block.
    Q_INVOKABLE void checkComputer();
    // A tone, sweep or noise into the selection on the focused track (replacing
    // it), or at the playhead for spec.seconds. One Undo step.
    Q_INVOKABLE bool generateAudio(const QVariantMap& spec);
    // Sets each selected clip's gain so its own playback reaches targetLufs.
    Q_INVOKABLE bool matchClipLoudness(double targetLufs, const QString& stamp);
    // Speech alignment (backend_speech.cpp): the clip under the playhead (or the
    // only clip) on alignTrack is re-timed to follow the one on referenceTrack, on a
    // worker; the result goes on a new track in one Undo step.
    Q_INVOKABLE QVariantMap speechAlignmentTarget(int referenceTrack, int alignTrack) const;
    Q_INVOKABLE bool alignSpeech(int referenceTrack, int alignTrack, double tightness, const QString& stamp);
    Q_INVOKABLE void cancelSpeechAlignment();
    // Remix (backend_speech.cpp): re-arranges the music clip under the playhead on
    // the focused track to targetSeconds on a worker, replacing it in one Undo step.
    Q_INVOKABLE QVariantMap remixTarget() const;
    Q_INVOKABLE bool remixClip(double targetSeconds, const QString& stamp);
    Q_INVOKABLE void cancelRemix();
    // The loudest absolute sample in the selection, envelopes included; 0 when it
    // is silent or empty.
    Q_INVOKABLE double selectionPeak() const;
    // Pastes the clipboard onto a new track at the start of the timeline.
    Q_INVOKABLE void pasteToNewTrack();
    Q_INVOKABLE void fadeIn();
    Q_INVOKABLE void fadeOut();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // --- projects ------------------------------------------------------------------

    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(const QString &path);
    Q_INVOKABLE bool collectProject(const QString& path);
    Q_INVOKABLE void cancelCollection();
    [[nodiscard]] bool collecting() const { return m_collecting; }
    [[nodiscard]] QVariantMap collectionStatus() const;
    Q_INVOKABLE void loadProject(const QString &path);
    [[nodiscard]] QVariantList recentDocuments() const;
    [[nodiscard]] QString recentDocumentsError() const { return m_recentDocumentsError; }
    Q_INVOKABLE bool reloadRecentDocuments();
    Q_INVOKABLE bool forgetRecentDocument(const QString &path, bool project);
    Q_INVOKABLE bool clearRecentDocuments();
    Q_INVOKABLE void newProject();
    // Complete a coherent plug-in check before the shell decides whether a
    // document change needs an unsaved-changes prompt. Requests may be superseded.
    Q_INVOKABLE void prepareDocumentChange(qulonglong request);

    // --- autosave and recovery (A22) -------------------------------------------------

    [[nodiscard]] int autosaveSeconds() const { return m_autosaveSeconds; }
    void setAutosaveSeconds(int seconds);
    [[nodiscard]] bool vst3Discovery() const { return m_vst3Discovery; }
    void setVst3Discovery(bool on);
    [[nodiscard]] bool vst3DiscoveryAvailable() const;
    [[nodiscard]] bool autosaving() const { return m_autosaving; }
    [[nodiscard]] QString lastAutosave() const { return m_lastAutosave; }
    [[nodiscard]] bool recoveryAvailable() const { return m_recoveryOffer.has_value(); }
    [[nodiscard]] QString recoveryName() const;
    [[nodiscard]] QString recoveryWritten() const;
    // The write the timer would make, now. Refused in words while the slot is busy.
    Q_INVOKABLE void autosaveNow();
    // Takes the offered recovery as the session: dirty, at the project's own path if
    // it had one. Or lets it go.
    Q_INVOKABLE void recoverSession();
    Q_INVOKABLE void discardRecovery();
    // Called after the UI is destroyed, before a graphics restart. Stops workers
    // and writes a checkpoint even with autosave off. A failed write forbids restart.
    // Empty index means there was no loaded session. This Backend is then quiescent.
    [[nodiscard]] bool prepareGraphicsRestart(QString &index);
    // Select only the checkpoint handed over by the previous process. The existing
    // recovery UI can restore it without accidentally offering another session.
    [[nodiscard]] bool offerGraphicsRecovery(const QString &index);
    // $XDG_STATE_HOME/omaaudio: where an unsaved session's recovery bundle lives, and
    // the index of what each run wrote. Public so the suites can point it somewhere.
    [[nodiscard]] static QString stateDir();

    // --- plugins -------------------------------------------------------------------

    Q_INVOKABLE void listPlugins();
    Q_INVOKABLE bool addPlugin(const QString &uri, int track = -1);
    [[nodiscard]] QStringList favoritePluginUris() const { return m_pluginLibrary.favorites(); }
    [[nodiscard]] QStringList recentPluginUris() const { return m_pluginLibrary.recent(); }
    [[nodiscard]] QStringList disabledPluginUris() const { return m_pluginLibrary.disabled(); }
    Q_INVOKABLE bool setPluginFavorite(const QString &uri, bool favorite);
    Q_INVOKABLE bool setPluginEnabled(const QString &uri, bool enabled);
    Q_INVOKABLE bool clearRecentPlugins();
    // What the Preset menu offers for an effect: {available, reason, name, uri,
    // stamp, fields, presets (the person's own), builtIn (OmaAudio's starting
    // points: {id, name, group, note, skipped}), pluginPresets (the effect's own
    // LV2 presets: {id, name})}. applyPluginPreset takes any of their ids.
    Q_INVOKABLE QVariantMap pluginPresetContext(int track, int slot);
    // The same for an effect on a clip, and applying one there: one Undo step.
    // A preset that keeps an effect's own complete settings is refused on a clip.
    Q_INVOKABLE QVariantMap clipEffectPresetContext(int track, int clip, int slot);
    Q_INVOKABLE bool applyClipEffectPreset(int track, int clip, int slot, const QString &id, const QString &stamp);
    Q_INVOKABLE QString savePluginPreset(int track, int slot, const QString &stamp, const QString &name);
    // A complete preset also keeps the effect's own settings (VST3/CLAP state),
    // captured safely from the running effect. Finishes with pluginPresetSaved.
    Q_INVOKABLE bool savePluginPresetWithState(int track, int slot, const QString &stamp, const QString &name);
    Q_INVOKABLE bool applyPluginPreset(int track, int slot, const QString &stamp, const QString &id);
    Q_INVOKABLE bool deletePluginPreset(const QString &id);
    Q_INVOKABLE void removePlugin(int slot, int track = -1);
    Q_INVOKABLE void movePlugin(int from, int to, int track = -1);
    Q_INVOKABLE void setPluginBypass(int slot, bool on, int track = -1);
    // The effect's own window (backend_plugin_editors.cpp). Live track and master
    // effects only; clip effects use their controls in the Clip Effects panel.
    Q_INVOKABLE bool pluginHasEditor(int slot, int track = -1);
    Q_INVOKABLE bool openPluginEditor(int slot, int track = -1);
    Q_INVOKABLE void closePluginEditor(int slot, int track = -1);
    Q_INVOKABLE bool pluginEditorOpen(int slot, int track = -1) const;
    [[nodiscard]] QString automationStamp() const;
    Q_INVOKABLE QVariantList automationTargets(int track = -1);
    [[nodiscard]] QString automationMode() const { return m_automationMode; }
    [[nodiscard]] int automationTrack() const { return m_automationTrack; }
    [[nodiscard]] QString automationKey() const { return m_automationKey; }
    [[nodiscard]] bool automationWriting() const { return m_automationPass.has_value(); }
    [[nodiscard]] double automationLiveValue() const { return m_automationValue; }
    [[nodiscard]] QString automationLiveStatus() const;
    [[nodiscard]] double automationReturnSeconds() const { return m_automationReturnSeconds; }
    void setAutomationReturnSeconds(double seconds);
    Q_INVOKABLE bool configureAutomation(int track, const QString& key, const QString& mode);
    Q_INVOKABLE void touchAutomation(bool pressed);
    Q_INVOKABLE void writeAutomationValue(double value);
    Q_INVOKABLE void finishAutomationRecording(bool cancel = false);
    Q_INVOKABLE void automationGesture(int track, const QString& key, bool pressed);
    Q_INVOKABLE bool editAutomation(const QString& key, const QString& operation,
        qint64 frame, qint64 to, double value, const QString& stamp = {}, int track = -1);
    Q_INVOKABLE void setPluginControl(int slot, qint64 port, double value, int track = -1);
    Q_INVOKABLE void requestPluginPorts(int slot, int track = -1);
    [[nodiscard]] static QString automationTiming(const lv2::PluginInfo *info);

    // The installed user manual, split at its headings for the Manual panel:
    // { available, path, error, sections: [{ title, level, text }] }. Read once
    // and kept, because it is a file that does not change while we run.
    Q_INVOKABLE QVariantMap manual() const;
    Q_INVOKABLE QString manualHtml(const QString &markdown, const QFont &font, const QColor &linkColor) const;
    // Publishes control writes made since the last notify. Called on release
    // rather than per drag tick, because notifying resets the rack's model.
    Q_INVOKABLE void flushPluginPorts() { emit pluginPortsChanged(); }

    // Pure, and static so the tests can call it without a window.
    [[nodiscard]] static double dbFromLinear(float linear);
    [[nodiscard]] static float linearFromDb(double db);
    [[nodiscard]] static QString userKeymapPath();

signals:
    void automationStampChanged();
    void recentDocumentsChanged();
    void automationLiveChanged();
    void automationReturnChanged();
    void duckingChanged();
    void clipEnvelopeEditFinished(int track, int clip, const QString& stamp, bool success, const QString& message);
    void clipEffectsChanged();
    void pluginEditorsChanged();
    void pluginPresetSaved(const QString& id, const QString& error);
    void recordingChanged();
    void recordingInputsChanged();
    void loopChanged();
    void clipMoveChanged();
    void masterChanged();
    void loudnessChanged();
    void previewChanged();
    void analysisChanged();
    void trackLoudnessChanged();
    void renderQueueChanged();
    void fileBatchChanged();
    void collectionChanged();
    // A file was imported with a reservation worth showing (truncated, damaged).
    void importWarning(const QString &text);
    void sessionImportChanged();
    void audioSetupChanged();
    void importPlacementChanged();
    void metadataChanged();
    void videoChanged();
    void videoTimecodeChanged();
    void videoFrameChanged();
    // One render left the queue: state is done, failed or cancelled.
    void renderFinished(int id, const QString &state, const QString &path, const QString &message);
    void renderProgressChanged();
    void spectralChanged();
    void spectralConfigChanged();
    void spectralProgressChanged();
    void sampleEditChanged();
    void effectPreviewChanged();
    void repairChanged();
    void noisePrintChanged();
    void maskChanged();
    void clicksChanged();
    void autosaveChanged();
    void recoveryChanged();
    void markersChanged();
    void snappingChanged();
    void rippleChanged();
    void masterGainChanged();
    void metronomeChanged();
    // After tracksChanged: the ids whose published data actually differs, so a view
    // of one track re-reads only when its own track changed.
    void trackDataChanged(const QVariantList& ids);
    void trackSummariesChanged();
    void computerCheckChanged();
    void speechAlignmentChanged();
    void remixChanged();
    void recipesChanged();
    void recipeRunChanged();
    void recordedStepsChanged();
    void pausesChanged();
    void pluginResponseChanged();
    void readyChanged();
    void projectSampleSettingsChanged();
    void storageChanged();
    void memoryChanged();
    void spectralStoreChanged();
    void timeDisplayChanged();
    void musicalTimeChanged();
    void lastErrorChanged();
    // Something worth saying that is not an error, such as an effect that left
    // the selection exactly as it was.
    void notice(const QString& text);
    void loadingChanged();
    void takeChanged();
    void timelineChanged();
    void statusChanged();
    void tracksChanged();
    void selectionChanged();
    void pluginsChanged();
    void pluginLibraryChanged();
    void effectMenuChanged();
    void pluginPortsChanged();
    void pluginValuesChanged();
    void pluginTextChanged();

    // Ctrl+S on a session that has never been saved: the window asks for a path
    // rather than the engine guessing one.
    void pathNeeded();
    void documentChangePrepared(qulonglong request, bool succeeded, bool changes);

    // The user's keymap file changed on disk.
    void keymapChanged();

    // Emitted by the worker thread, connected queued, so the slot runs on the GUI
    // thread. This is the whole of the old `server_.wake()` mechanism.
    void workerFinished();

private slots:
    void drainWorker();
    void pollStatus();

private:
    // The integration suite drives real callback blocks without an audio device.
    friend struct BackendTestAudio;
    static constexpr std::uint32_t kOutputChannels = 2;
    struct LoadedTake {
        std::uint64_t sessionGeneration = 0;
        std::uint64_t editRevision = 0;     // the session as it was when the load began
        std::uint32_t projectRate = 0;      // the rate the audio was converted for
        bool ok = false;
        bool replaceProject = false;
        std::string error;
        std::string path;
        std::string name;
        session::AudioBuffer audio;
        std::uint32_t channels = 0;
        std::uint32_t originalRate = 0;
        bool resampled = false;
        std::string warning;        // read with a reservation (truncated, damaged)
        std::string formatName;
        session::AudioMetadata metadata;   // as the file carried it, at the file's rate
    };

    struct PeakSet {
        QString error;
        std::uint64_t sessionGeneration = 0;
        bool ok = false;
        session::TrackId track = session::kInvalidTrack;
        io::PeakPyramid pyramid;
        std::vector<io::PeakTexture> textures;
        DisplayCache::Lease cache;
    };

    enum class Job { None, Load, Peaks, Save, LoadProject, Scan, Analyse, Render, Spectral, Repair, Autosave, Detect, Pitch, SampleEdit, Storage, NoisePrint, TrackLoudness, Ducking };

    struct DuckingJob {
        std::uint64_t generation = 0, revision = 0, analysisEpoch = 0, serial = 0;
        session::TrackId track = session::kInvalidTrack;
        std::vector<session::TrackId> triggers;
        render::DuckingSettings settings;
        session::AutomationCurve baseline;
        // The curve an Apply replaces: the baseline, or when rebuilding a
        // stored recipe the curve that recipe generated.
        session::AutomationCurve replaces;
        render::DuckingResult result;
        bool follow = false;                       // regenerating a stored recipe
        bool rebuild = false;                      // new settings on a stored recipe's baseline
        std::uint64_t undoVersion = 0;             // amend the causing edit if unchanged
        std::vector<session::TrackState> inputs;   // the trigger audio it describes
    };
    void finishDucking(DuckingJob result);
    // Generated ducking that follows its triggers (backend_ducking.cpp).
    [[nodiscard]] std::optional<std::vector<session::TrackState>> duckingInputs(
        const session::Project& project, const session::DuckingRecipe& recipe) const;
    void scheduleDuckingFollow();
    void followGeneratedDucking();
    void rememberDuckingInputs();
    void finishFollowedDucking(DuckingJob job);
    std::map<session::TrackId, std::vector<session::TrackState>> m_duckingInputs;
    QTimer* m_duckingFollowTimer = nullptr;
    bool m_followingDucking = false;

    struct TrackLoudnessJob {
        std::uint64_t generation = 0, revision = 0;
        std::uint64_t analysisEpoch = 0;
        session::TrackId track = session::kInvalidTrack;
        render::MixMeasurement measurement;
    };
    void finishTrackLoudness(TrackLoudnessJob result);

    // What an offline master render produced, for the Mastering page.
    struct Analysis {
        bool ok = false;
        std::uint64_t generation = 0, revision = 0;
        std::uint64_t analysisEpoch = 0;
        std::string error;
        PeakSet peaks;
        engine::LoudnessReport report;
        engine::LoudnessReport rawReport;   // the plain mix, for the level match
        std::vector<engine::LoudnessPoint> history;
        qint64 frames = 0;
        std::uint32_t hop = 0;
    };

    struct RenderJob {
        int id = -1;
        QString name;
        render::ExportSpec spec;
        QString state;          // queued | rendering | done | failed | cancelled
        std::uint64_t generation = 0;   // the session it was queued for
        double progress = 0.0;
        QString message;
        QString warning;
        QVariantMap report;
        // A stem: this track's own output (effects, fader, pan and the tracks
        // feeding it), without the master bus. Invalid for the full mix.
        session::TrackId stemTrack = session::kInvalidTrack;
        // A stem the user chose by name is rendered even when its track is muted:
        // asking for it is the reason to hear it.
        bool stemChosen = false;
        // A time range of the project, in project frames: the file holds exactly
        // these frames of the whole render. Both -1 for the whole project.
        session::FramePos rangeStart = -1;
        session::FramePos rangeEnd = -1;
    };

    struct RenderOutcome {
        int id = -1;
        render::ExportResult result;
    };

    // Which chain a plugin call means: a track's, or the master bus's.
    struct ChainTarget {
        std::vector<session::Insert> *inserts = nullptr;
        session::EffectOwner key{};
        session::TrackId track = session::kInvalidTrack;
        int target = -1;        // what QML passed, resolved: a track id or kMasterTarget
    };
    static constexpr session::EffectOwner kMasterChainKey = session::EffectOwner::master();

    struct SaveJob {
        std::uint64_t sessionGeneration = 0;
        std::uint64_t revision = 0;
        bool ok = false;
        std::string error;
        std::string path;
    };

    // One run's line in the recovery index: which bundle it autosaves into, the
    // project that bundle stands for (empty for a session never saved), and enough
    // to say so in a prompt.
    struct RecoveryEntry {
        QString file;           // the index file itself
        QString bundle;
        QString project;
        QString name;
        QString written;        // ISO 8601, UTC
        qint64 pid = 0;
    };

    struct ProjectLoadRequest {
        QString path;
        bool sessionImport = false;
        QString mediaFolder;
        std::uint64_t sessionGeneration = 0;
        std::uint64_t revision = 0;
        std::uint64_t hostRevision = 0; // ordinary edits, excluding live plug-in notifications
        std::optional<RecoveryEntry> recovery;
    };

    struct PluginCatalogueResult {
        std::unique_ptr<lv2::World> world;
        QStringList warnings;
        bool complete = false, cancelled = false;
    };
    [[nodiscard]] static vst3::CatalogueOptions defaultVst3ScanOptions();
    [[nodiscard]] static PluginCatalogueResult scanPluginCatalogue(
        const std::optional<vst3::CatalogueOptions>& options, const std::atomic<bool>& cancel);
    void acceptPluginCatalogue(PluginCatalogueResult result);
    void publishPluginCatalogue();

    struct ProjectLoadResult {
        ProjectLoadRequest request;
        io::LoadResult result;
        PluginCatalogueResult catalogue;
        QString importMessage, importName, importFormat = QStringLiteral("AAF");
    };
    QString m_sessionImportMessage;

    struct SpectralResult {
        std::uint64_t generation = 0;
        bool midSide = false;
        bool ok = false;
        std::string error;
        session::TrackId track = session::kInvalidTrack;
        std::uint32_t sequence = 0;
        spectral::SpectralSet set;
        DisplayCache::Lease cache;
    };

    struct NoisePrintRequest {
        std::uint64_t generation = 0, revision = 0, sequence = 0;
        session::TrackId track = session::kInvalidTrack;
        session::FramePos from = 0, to = 0;
        spectral::StftConfig config;
        QString scope, name;
    };
    struct NoisePrintJob {
        NoisePrintRequest request;
        spectral::NoisePrint print;
        QString summary, error;
        bool cancelled = false;
    };
    void pumpNoisePrint();
    void finishNoisePrint(NoisePrintJob);

    struct DetectResult {
        bool cancelled = false;
        QString error;
        session::TrackId track = session::kInvalidTrack;
        session::FramePos from = 0;             // where the buffer started
        std::vector<spectral::Click> clicks;    // in the take's frames
        session::SessionState expected;
        bool wholeTake = false;
        bool thenHeal = false;
    };

    struct RepairResult {
        bool ok = false, cancelled = false, noChange = false;
        QString summary;
        std::uint64_t generation = 0, revision = 0;
        std::string error;
        std::string label;
        session::TrackId track = session::kInvalidTrack;
        session::FramePos start = 0;
        session::AudioBuffer audio;         // the repaired [start, start + frames)
        // What the session looked like when the repair started; if it differs when
        // the result lands, the result is for a take that no longer exists.
        session::SessionState expected;
        // The clicks still listed after this one is healed (healClickAt).
        std::vector<spectral::Click> keepClicks;
    };

    enum class SampleEditKind { Reverse, Normalize, VoiceCleanup, Mono, Stereo, CrossfadeLinear, CrossfadePower, Stretch, GroupStretch,
                                AdjustCrossfadeLinear, AdjustCrossfadePower, Channels, MapChannels, InsertSilence, PasteNew, ProjectRate, MultitrackPaste,
                                ApplyEffect };
    struct SampleEditRequest {
        SampleEditKind kind = SampleEditKind::Reverse;
        session::TrackId track = session::kInvalidTrack;
        session::Selection selection;
        std::uint64_t generation = 0, revision = 0;
        qint64 targetFrames = 0;
        std::size_t clipIndex = 0;
        qint64 rangeStart = 0, rangeEnd = 0;
        std::optional<session::ChannelEditSpec> channelSpec;
        std::vector<std::vector<float>> channelMatrix;
        bool allTracks = false;
        QString label;
        float voiceMix = 1;
        std::optional<session::MultitrackClipboard> multitrack;
        bool ripple = false;
        std::uint64_t selectionRevision = 0;
        std::vector<session::ClipAddress> clips{};
    };
    struct SampleEditJob {
        SampleEditRequest request;
        session::SampleEditResult result;
        std::vector<session::ClipAddress> clips{};
    };
    enum class StorageKind { Prune, Inspect, Remove };
    struct StorageRequest {
        StorageKind kind = StorageKind::Inspect;
        std::uint64_t generation = 0, revision = 0;
        QString path;
        std::vector<session::SourceId> protectedIds;
        std::vector<std::string> protectedUuids;
        std::vector<io::UnusedAudioFile> files;
    };
    struct StorageJob {
        StorageRequest request;
        std::optional<session::Project> project;
        io::AudioCleanupPreview preview;
        io::AudioCleanupResult cleanup;
        std::size_t pruned = 0;
        bool cancelled = false;
        QString error;
    };
    [[nodiscard]] std::vector<session::SourceId> protectedSourceIds() const;
    void startStorage(StorageKind kind, const QString& stamp = QString());
    void pumpStorage();
    void finishStorage(StorageJob job);
    void startSampleEdit(SampleEditKind kind, qint64 targetFrames = 0, std::size_t clipIndex = 0,
                         qint64 rangeStart = 0, qint64 rangeEnd = 0,
                         std::optional<session::ChannelEditSpec> spec = std::nullopt,
                         std::vector<std::vector<float>> matrix = {}, bool allTracks = false,
                         float voiceMix = 1);
    void startChannelEdit(session::ChannelEdit operation, double gain = 1, qint64 at = -1,
                          double incomingGain = 1, bool newTrack = false);
    static QString sampleEditName(SampleEditKind kind);
    void pumpSampleEdit();
    void finishSampleEdit(SampleEditJob job);
    [[nodiscard]] bool sampleEditContextCurrent(const SampleEditRequest& request) const;
    void startMultitrackPaste(qint64 frame, bool newTracks);
    bool copyTrackSelection();
    void deleteTrackSelection(bool ripple, const std::string& label);
    void removeTrackIds(const std::vector<session::TrackId>& ids);
    [[nodiscard]] std::vector<session::TrackId> selectedTrackIds() const;
    [[nodiscard]] session::FramePos selectedTrackLength() const;
    [[nodiscard]] bool selectedClipsCurrent() const;
    [[nodiscard]] QString clipPropertySelectionStamp() const;
    bool applySingleClipProperties(const session::ClipAddress& target, double gainDb, bool replaceFades,
                                   qint64 fadeIn, qint64 fadeOut, const std::optional<session::ClipFadeShapes>& shapes);
    bool applyClipPropertyEdit(const std::vector<session::ClipAddress>& clips, const session::ClipPropertyEdit& edit);
    bool applyClipEnvelopeEdit(const session::ClipAddress& target, const session::Clip& replacement,
                               const QString& stamp);
    void applyClipSelection(std::vector<session::ClipAddress> clips, session::TrackId focus,
                            std::size_t focusedClip);
    bool copyClipSelection();
    void deleteClipSelection(const std::string& label);
    bool applySelectedStretch(const std::vector<session::ClipAddress>& clips, qint64 frames,
                              bool realtime, double pitchChange, std::uint64_t serial);
    void remapStretchedSelection(const std::vector<session::ClipAddress>& before,
                                const std::vector<session::ClipAddress>& after);
    bool applyClipTranslation(const std::vector<session::ClipAddress>& clips, qint64 deltaFrames,
                              int deltaTracks, session::TrackId focus, std::size_t focusedClip);
    enum class ClipGroupEdit { Create, Update, Detach, Dissolve };
    bool applyClipGroupEdit(ClipGroupEdit operation, const std::vector<session::ClipAddress>& clips,
                            const session::ClipGroup& expected, const QString& name, const QString& colour);

    void refreshLoop();
    struct ClipMove {
        session::TrackId from = session::kInvalidTrack, to = session::kInvalidTrack;
        std::size_t index = 0;
        session::Clip clip;
        qint64 position = 0;
        std::uint64_t generation = 0, revision = 0;
        bool slip = false;
        qint64 sourceOffset = 0;
        std::vector<session::ClipAddress> group;
        std::uint64_t selectionRevision = 0;
    };
    [[nodiscard]] bool clipMoveCurrent() const;
    bool applyClipMove(const ClipMove& move);
    bool m_loopEnabled = false;
    qint64 m_loopStart = 0, m_loopEnd = 0;
    std::optional<ClipMove> m_clipMove;
    QVariantMap m_clipMovePreview;
    void setError(const QString &message);
    // Everything saved beside the Project: focus, selection, and the Spectral
    // page's band, mask and noise print.
    [[nodiscard]] io::ProjectView currentView() const;
    // A loaded bundle becomes the session. `projectPath` is what Save writes to
    // (empty = ask), `dirty` is whether it needs saving already (a recovery does).
    void installLoaded(io::LoadResult result, const QString &projectPath, bool dirty);
    void pumpSave();
    enum class SnapshotResult { Ready, Pending, Failed };
    SnapshotResult captureProjectSnapshot(session::Project& snapshot, QString& error);
    // Accepted delivery jobs own their model and the exact processor lifetimes
    // they capture. GUI commands after each request wait behind its boundary.
    struct ManualAutomationInput {
        double value = 0;
        std::uint64_t revision = 0, generation = 0;
        std::uint32_t playbackEpoch = 0;
    };
    struct DeliverySnapshot {
        session::Project project;
        io::ProjectView view;
        std::uint64_t generation = 0, revision = 0;
        std::uint64_t analysisEpoch = 0;
        std::uint64_t automationRun = 0;
        std::optional<ManualAutomationInput> manualAutomation{};
        struct Chain { session::EffectOwner key; std::shared_ptr<lv2::Chain> processor; };
        std::vector<Chain> chains;
        bool includeBypassed = true;
        bool includeInputControls = false;
        bool finishAutomation = false;
        std::function<void()> owned;
        std::deque<engine::Command> commandsBefore;
        std::function<bool()> cancelled;
        std::function<void(DeliverySnapshot, QString, bool)> complete;
    };
    void requestDeliverySnapshot(std::optional<session::EffectOwner> chain, bool includeBypassed,
                                std::function<bool()> cancelled,
                                std::function<void(DeliverySnapshot, QString, bool)> complete,
                                bool includeInputControls = false, bool finishAutomation = false,
                                std::function<void()> owned = {}, bool timeline = false);
    // clipEffects: the snapshot's project plays its clip effects (rendered first);
    // false keeps the editable model, e.g. for collecting a project.
    void requestTimelineSnapshot(std::function<bool()> cancelled,
                                 std::function<void(DeliverySnapshot, QString, bool)> complete,
                                 bool clipEffects = true);
    // --- clip effect rendering (backend_clip_effects.cpp) ---
    struct ClipEffectJob {
        render::ClipEffectRegion region;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    struct ClipEffectWaiter {
        DeliverySnapshot snapshot;
        std::function<void(DeliverySnapshot, QString, bool)> complete;
        std::vector<render::ClipEffectRegion> regions;
    };
    render::ClipEffectRenders m_clipEffectRenders;
    std::vector<ClipEffectJob> m_clipEffectJobs;
    std::vector<std::pair<render::ClipEffectRegion, QString>> m_clipEffectFailures;
    std::vector<ClipEffectWaiter> m_clipEffectWaiters;
    QThreadPool m_clipEffectPool;
    mutable std::map<std::pair<std::string, unsigned>, QVariantList> m_clipEffectPorts;
    // The model as heard: silent where a rendering is still on its way.
    [[nodiscard]] render::AppliedClipEffects heardProject();
    void requestClipEffectRenders(const std::vector<render::ClipEffectRegion>& regions,
                                  const session::Project& project, bool retryFailed = false);
    void finishClipEffectRender(const render::ClipEffectRegion& region,
                                std::shared_ptr<render::ClipEffectRender> result);
    void awaitClipEffects(DeliverySnapshot snapshot, std::function<void(DeliverySnapshot, QString, bool)> complete);
    void resumeClipEffectWaiters();
    void cancelClipEffectRenders(bool all);
    bool replaceClipRack(int track, int clip, const QString& stamp, const std::string& label,
                         const std::function<bool(std::vector<session::Insert>&, QString&)>& change);
    bool captureTimelineAutomation(DeliverySnapshot& snapshot, QString& error);
    void pumpDeliverySnapshots();
    void verifyLoadRevision(std::uint64_t generation, std::uint64_t revision,
                            std::function<void(QString)> complete);
    [[nodiscard]] bool deliveryBoundaryActive() const;
    [[nodiscard]] bool hasPendingTimelineSnapshot() const;
    void startCollectedSnapshot(DeliverySnapshot snapshot, const QString& target);
    void startCapturedTrackLoudness(DeliverySnapshot snapshot, session::TrackId track, QString error, bool cancelled);
    void startCapturedDucking(DeliverySnapshot snapshot, DuckingJob job, QString error, bool cancelled);
    void retrySnapshot(int milliseconds = 1);
    void cancelSnapshot();
    void pumpProjectLoad();
    void finishProjectLoad(ProjectLoadResult result, bool verified = false, QString verificationError = {});
    void prepareSessionImport(ProjectLoadResult result);
    void pumpWorkerJobs();
    void resetSessionJobs();
    void armAutosave();
    void autosave(bool synchronous);
    void finishAutosave(SaveJob job);
    void scanRecovery(const QString &preferredIndex = {});
    void quiesceForShutdown();
    void dropRecovery();
    [[nodiscard]] QString recoveryBundle();
    [[nodiscard]] QString recoveryIndexPath() const;
    [[nodiscard]] static std::string writeRecoveryIndex(const std::string &indexPath,
                                                        const RecoveryEntry &entry);
    [[nodiscard]] static SaveJob writeRecovery(const session::Project &project,
                                               const io::ProjectView &view,
                                               const std::string &bundle,
                                               const std::string &indexPath,
                                               const RecoveryEntry &entry);
    void beginLoad(const QString &path, bool replaceProject);
    void finishAudioLoad(LoadedTake take, bool verified = false, QString verificationError = {});
    void installTake(LoadedTake take);
    void publishPlan(bool curvesOnly = false, bool audibleChange = true);
    void previewAutomation(engine::RenderPlan& plan) const;
    void pollAutomation();
    void sampleAutomation(qint64 frame, std::uint64_t loopWraps = 0);
    [[nodiscard]] lv2::Chain* processAutomationChain();
    void sampleProcessAutomation(const host::ParameterTime& time);
    session::AutomationCurve* liveAutomationCurve(bool create = false);
    session::SessionState captureEditState();
    bool writeMatchingAutomation(int track, const QString& key, double value);
    void collectRetiredPlans();
    void publishTrackParams();
    void rebuildTracks(bool notifyUnchanged = true);
    void refreshTrackMixControls(int track);
    void rebuildMarkers();
    [[nodiscard]] session::Marker *markerArg(int id);
    bool addPreparedMarker(session::Marker marker, const std::string& label);
    void markPeaksDirty(session::TrackId track);
    void pumpPeaks();
    void joinWorker();
    [[nodiscard]] PeakSet buildPeaks(const std::vector<session::Clip> &clips,
                                     std::uint32_t sequence,
                                     session::TrackId track) const;
    void installPeaks(PeakSet set);
    bool writePeakTextures(PeakSet &set, const QString &stem) const;
    void removeTextures(PeakSet &set);

    [[nodiscard]] session::Track *focusedTrackPtr();
    [[nodiscard]] std::vector<session::Clip> *focusedClips();
    [[nodiscard]] session::FramePos trackLength(session::TrackId id) const noexcept;
    [[nodiscard]] session::FramePos snapToZeroCrossing(session::FramePos frame) const;
    [[nodiscard]] std::optional<qint64> nearestMusicalGridFrame(qint64 frame) const;
    void clampSelection();
    void commit(std::string label, session::TrackId track,
                const session::SessionState &before);
    [[nodiscard]] std::vector<session::TrackId> changedWaveforms(const session::SessionState &state) const;
    void installState(const session::SessionState &state, session::TrackId focus);
    void markDirty(bool pluginNotification = false);
    void rebuildChain(session::TrackId id);
    bool rebuildChainFor(session::EffectOwner key, std::vector<session::Insert> *inserts, bool restarting = false);
    void rebuildAllChains();
    [[nodiscard]] ChainTarget chainTarget(int track);
    [[nodiscard]] ChainTarget chainTarget(session::EffectOwner owner);
    void rebuildMaster();
    void invalidateAnalysis(bool readout = false);
    [[nodiscard]] bool analysisCurrent(std::uint64_t generation, std::uint64_t revision, std::uint64_t epoch) const;
    void refreshAnalysisValidity();
    void queueAnalysisValidityRefresh();
    void pollLoudness();
    void pushPreview();
    // Instantiate only; its exclusively owning worker must warm before rendering.
    // Null with an empty error means no inserts; active failures reject the whole job.
    [[nodiscard]] std::unique_ptr<lv2::Chain> buildOfflineChain(const std::vector<session::Insert> &inserts,
                                                             lv2::HostFeatures& features, QString& error);
    void startMasterJob(bool exportIt, int jobId);
    void startCapturedMasterJob(DeliverySnapshot snapshot, bool exportIt, int jobId, QString error, bool cancelled);
    void installAnalysis(Analysis analysis);
    void finishRender(RenderOutcome outcome);
    void pumpRenders();
    [[nodiscard]] RenderJob *renderJob(int id);
    bool fileBatchOptions(const QVariantMap& settings, FileBatchProcessor::Options& options, QString& error) const;
    void initialiseFileBatch();
    [[nodiscard]] QVariantMap renderJobToMap(const RenderJob &job) const;
    [[nodiscard]] static QVariantMap reportToMap(const engine::LoudnessReport &report);
    [[nodiscard]] lv2::HostFeatures &hostFeatures();
    bool prepareProjectRate(std::uint32_t rate);
    void rescaleProjectView(std::uint32_t oldRate, bool preservePosition);
    void finishProjectRate(bool restart, std::uint32_t oldRate, std::uint32_t oldBits);
    [[nodiscard]] session::TrackId trackArg(int track) const;

    [[nodiscard]] spectral::StftConfig stftConfig() const;
    [[nodiscard]] int trackChannels(const session::Track &track) const;
    void markSpectralStale();
    void pumpSpectral();
    void installSpectral(SpectralResult result);
    void clearSpectral();
    enum class RepairKind { Attenuate, Heal, Denoise, Clicks, Declip, Dereverb };
    struct RepairRequest {
        RepairKind kind = RepairKind::Attenuate;
        double gainDb = 0.0;
        double thresholdDb = -1.0, decaySeconds = 0.6;
        spectral::ChannelScope scope = spectral::ChannelScope::All;
        std::uint64_t generation = 0, revision = 0;
        session::TrackId requestedTrack = session::kInvalidTrack;
        session::Selection requestedSelection;
        session::SessionState requestedState;
        spectral::Band band;
        double amountDb = 0.0;
        double strength = 1.0;
        // What inside the region the repair touches: the drawn mask if there is
        // one (a spot heal brings its own), else the band.
        std::optional<spectral::MaskShapes> mask;
        // A region of its own instead of the selection (the spot tool).
        std::optional<std::pair<session::FramePos, session::FramePos>> region;
        // The clicks to take out, for Clicks; in the take's frames.
        std::vector<spectral::Click> clicks;
        // The clicks to keep listed once this one has landed (healClickAt).
        std::vector<spectral::Click> keepClicks;
        // The whole spectrum on every channel, whatever mask, band and channel
        // the Spectral page holds (the Sound tab's Room echo).
        bool plain = false;
    };
    void setClicks(std::vector<spectral::Click> clicks, const QString &summary);
    void finishDetect(DetectResult result);
    void runDetect(double sensitivity);
    void pumpDetect();
    [[nodiscard]] static spectral::MaskOp maskOp(bool subtract, bool intersect) noexcept
    {
        return intersect ? spectral::MaskOp::Intersect
             : subtract ? spectral::MaskOp::Subtract : spectral::MaskOp::Add;
    }
    [[nodiscard]] static spectral::MaskShapes maskFromVariants(const QVariantList &points,
                                                                const QVariantList &stamps,
                                                                spectral::MaskOp op);
    void rebuildMaskShapes();
    struct PitchRequest {
        spectral::HarmonicComb comb;
        spectral::MaskOp op = spectral::MaskOp::Add;
        spectral::StftConfig config;
        session::TrackId track = session::kInvalidTrack;
        session::Selection selection;
        std::uint64_t generation = 0, revision = 0, maskRevision = 0;
        session::FramePos seed = 0;
        double bandLow = 0, bandHigh = 0;
    };
    struct PitchResult { PitchRequest request; bool cancelled = false; };
    [[nodiscard]] bool pitchContextCurrent(const PitchRequest& request) const;
    void pumpPitch();
    void finishPitch(PitchResult result);
    void selectMaskBounds();
    void startRepair(RepairRequest request);
    void runRepair(const RepairRequest &request);
    void pumpRepair();
    // The focused take's audio over [from, to), rendered here: what a repair or a
    // noise print works on. Empty when there is no take.
    [[nodiscard]] session::AudioBuffer renderFocusedRange(session::FramePos from,
                                                          session::FramePos to) const;
    void finishRepair(RepairResult result);

    bool m_ready = false;
    QString m_lastError;
    bool m_loading = false;
    bool m_loaded = false;
    QString m_takeName;
    QString m_takePath;
    int m_takeChannels = 0;
    int m_originalRate = 0;
    bool m_resampled = false;

    qint64 m_playhead = 0;
    bool m_playing = false;
    int m_playSpeed = 1;
    void startPlayback(int speed);
    double m_peakLeft = 0.0;
    double m_peakRight = 0.0;

    QVariantList m_tracks;
    session::TrackId m_focused = session::kInvalidTrack;

    void pollRecording();
    void finishRecording();
    void updateRecordingTiming();
    void scanRecordingRecovery();
    void rememberRecording(const QString& path);
    void retireRecordings(std::uint64_t generation, std::uint64_t revision);
    static QString recordingsDirectory();
    struct RecordingPlacement {
        session::TrackId target = session::kInvalidTrack;
        // The lane in focus when recording began: new take tracks go just under it.
        session::TrackId anchor = session::kInvalidTrack;
        session::FramePos start = 0, limit = 0;
        session::FramePos lead = 0, alignment = 0, residual = 0;
        session::FramePos passLength = 0;
        int passes = 0;
        bool align = false, alignmentReady = true;
        std::uint64_t generation = 0;
        std::vector<session::Clip> expected;
        QString path;
    } m_recordPlacement;
    struct CompletedRecording {
        engine::RecordingResult result;
        RecordingPlacement placement;
        QString input, error;
    };
    void installRecordings(std::vector<CompletedRecording> takes);
    struct AdditionalRecording {
        engine::CaptureClock clock;
        RecordingPlacement placement;
        QString input, error;
        unsigned channels = 1;
        std::unique_ptr<engine::Recording> writer;
        std::unique_ptr<engine::PwCapture> capture;
        QElapsedTimer progress;
        qint64 lastFrames = 0;
    };
    QVariantList m_additionalRecordInputs;
    std::vector<std::unique_ptr<AdditionalRecording>> m_additionalRecordings;
    struct RecordedSpool { QString path; std::uint64_t generation, revision; };
    std::vector<RecordedSpool> m_recordedSpools;
    std::unique_ptr<engine::Recording> m_recording;
    std::unique_ptr<engine::PwCapture> m_captureInput;
    QString m_recordingStatus, m_recordingError, m_recordInput;
    int m_recordArmedTrack = -1, m_recordChannels = 1;
    bool m_recordMonitor = false, m_recordPunch = false, m_recordStopping = false;
    double m_recordPreRoll = 0, m_recordTempo = 120, m_recordOffsetMs = 0;
    int m_recordCountIn = 0, m_recordLoopTakes = 0;
    bool m_recordAlign = true;
    QElapsedTimer m_captureProgress;
    qint64 m_lastCaptureFrames = 0;
    AudioCommand* m_inputScan = nullptr;
    QStringList m_recordingInputNames{QStringLiteral("System default input")};
    QStringList m_recordingInputIds{QString()};
    QSet<QString> m_jackInputIds;               // inputs that are JACK clients: linked by hand
    AudioCommand *m_jackLinkScan = nullptr;
    AudioLinks *m_jackInputLinks = nullptr;
    quint64 m_jackLinkGeneration = 0;
    int m_jackLinkAttempts = 0;
    void linkJackInput();
    QVariantList m_recoverableRecordings;

    DisplayCache m_displayCache;
    [[nodiscard]] std::string runtimeDir() const;
    // A synchronous legacy LV2/CLAP insert may precede the full worker scan.
    // Retain that world through every live/retired render plan and Transport.
    // Only the first successful full catalogue replaces it; no live rescan.
    std::vector<std::unique_ptr<lv2::World>> m_earlyCatalogues;
    session::Project m_project;
    engine::Transport::Queue m_commands;
    // GUI-owned overflow; only the bounded lock-free queue reaches the callback.
    std::deque<engine::Command> m_pendingCommands;
    engine::MeterChannel m_meters;
    engine::CaptureClock m_captureClock;
    engine::Transport m_transport;
    engine::PwOutput m_output;

    std::shared_ptr<engine::RenderPlan> m_currentPlan;
    std::vector<std::shared_ptr<engine::RenderPlan>> m_retiring;
    std::vector<session::Project> m_retiredProjects;
    // Releases acknowledged old sessions off the GUI thread; drained at shutdown.
    QThreadPool m_retirementPool;
    std::uint32_t m_planGeneration = 0;

    std::unordered_map<std::uint32_t, PeakSet> m_peaks;
    std::deque<session::TrackId> m_dirtyPeaks;
    std::uint32_t m_peakSequence = 0;
    std::atomic<bool> m_cancelPeaks{false};

    std::thread m_worker;
    std::atomic<bool> m_busy{false};
    std::mutex m_mutex;
    // Every finished job, in order. A single "the finished job" field was a race:
    // the worker clears m_busy before the GUI thread has drained, so a job started
    // in that gap overwrote the marker and the earlier result was never installed
    // (found by the shell suite: a spectral build flagged as running for ever).
    // Results queue with their jobs (a payload slot per type was the same race one
    // step later: two peak rebuilds finishing before one drain left a moved-from
    // set to install).
    std::deque<Job> m_finishedJobs;
    std::deque<LoadedTake> m_pendingLoad;
    std::deque<PeakSet> m_pendingPeaks;
    std::deque<SaveJob> m_pendingSave;
    std::deque<SaveJob> m_pendingAutosave;
    std::deque<ProjectLoadResult> m_pendingProjectLoad;
    std::deque<PluginCatalogueResult> m_pendingCatalogues;
    std::optional<ProjectLoadRequest> m_projectLoadRequest;
    bool m_projectLoadActive = false;
    bool m_sampleEditActive = false;
    bool m_groupStretchDeferred = false;
    std::uint64_t m_groupStretchCancelSerial = 0;
    QString m_automationMode = QStringLiteral("read"), m_automationKey;
    int m_automationTrack = -2;
    QVariantMap m_automationDescriptor;
    double m_automationValue = 1;
    bool m_automationTouch = false, m_automationLatched = false, m_automationSuspended = false;
    bool m_automationManualTouch = false, m_automationManualLatched = false;
    bool m_automationAppliedManualTouch = false;
    std::uint64_t m_automationRunToken = 0, m_automationManualRevision = 0;
    std::uint64_t m_automationArmRevision = 0;
    double m_automationManualValue = 1;
    bool m_automationInstances = false;
    std::array<session::InstanceAutomation, 2> m_automationInstanceRead{};
    void prepareInstanceAutomation();
    // User preference: each Touch release captures its own return duration.
    double m_automationReturnSeconds = 1;
    std::shared_ptr<omawave::engine::AutomationRecorder> m_manualAutomation;
    [[nodiscard]] bool processFaderAutomation() const;
    [[nodiscard]] omawave::lv2::Chain* processBlockAutomationChain() const;
    [[nodiscard]] bool processManualAutomation() const { return processFaderAutomation() || processBlockAutomationChain(); }
    [[nodiscard]] bool processOwnedAutomation() { return processManualAutomation() || processAutomationChain(); }
    void consumeManualAutomation();
    bool recordAutomationRamp(const omawave::host::ParameterEvent&, std::size_t actual);
    // Exclusive transport owner, after flushing plug-in input. Persist pending
    // intent at its next legal position without calling it rendered audio.
    bool capturePendingAutomation(QString& error, const std::optional<ManualAutomationInput>& manual);
    bool capturePendingAutomationInto(session::Insert& candidate, QString& error, bool& changed,
                                      std::uint64_t manualRevision = 0);
    bool capturePendingManualAutomationInto(session::Project&, const std::optional<ManualAutomationInput>&,
                                            QString& error, bool& changed);
    void acknowledgeManualAutomation(std::size_t actual, std::uint64_t revision);
    [[nodiscard]] bool pendingManualAutomation() const;
    void requestAutomationFinish(bool cancel = false);
    std::optional<bool> deferAutomationEdit(std::function<bool()> edit, std::function<void()> abandoned = {},
                                             bool keepCurrentView = false);
    [[nodiscard]] bool automationStampCurrent(const QString& stamp) const;
    void applyDeferredAutomationEdits();
    void discardDeferredAutomationEdits();
    struct DeferredAutomationEdit {
        std::function<bool()> apply;
        std::function<void()> abandoned;
        std::vector<session::SourceId> sources;
    };
    std::deque<DeferredAutomationEdit> m_deferredAutomationEdits;
    bool m_applyingAutomationEdit = false, m_automationEditQueueActive = false;
    QString m_automationEditAcceptedStamp;
    bool m_automationFinishPending = false, m_automationFinishOwned = false;
    bool m_automationFinishFailed = false, m_automationFinishCancel = false;
    bool m_automationUpdating = false;
    bool m_finishingAutomation = false;
    bool m_automationIndependentEdit = false;
    bool m_consumingPluginNotifications = false, m_automationPlanPending = false;
    bool m_automationPlanAudibleChange = false;
    bool m_automationCancelPending = false;
    void publishAutomationPlan(bool audibleChange = true);
    std::vector<std::size_t> m_nativeAutomationTouches;
    std::vector<std::pair<std::size_t, std::uint64_t>> m_nativeAutomationGroups;
    bool recordNativeAutomation(session::EffectOwner key, std::size_t slot, std::size_t actual,
                                const host::ParameterEvent& event);
    // A Write pass starts at the requested frame, before the audio callback.
    std::optional<std::uint32_t> m_automationPlayEpoch;
    struct AutomationPass {
        session::SessionState before;
        std::uint64_t generation = 0;
        qint64 lastFrame = -1;
        double lastValue = 1;
        std::uint64_t processSequence = 0, processLoopIteration = 0;
        bool followingRead = false;
        std::size_t processSource = SIZE_MAX;
        qint64 rampStart = -1, rampEnd = -1;
        double rampFrom = 0, rampStep = 0;
        std::uint32_t rampGrid = 1;
        struct Span {
            std::uint64_t sequence = 0, loop = 0;
            qint64 start = -1, end = -1;
            double from = 0, step = 0;
            std::uint32_t grid = 1;
            std::uint64_t acceptedRevision = 0;
        };
        std::array<Span, 2> spans{};
        std::array<std::optional<omawave::host::ParameterEvent>, 2> pending{};
        std::array<std::optional<omawave::host::UnrenderedParameter>, 2> pendingInputs{};
        // Accepted host input; its timestamp identifies the document/playback,
        // not a rendered frame. Resolve the lane position under audio ownership.
        std::optional<ManualAutomationInput> manualPending{};
    };
    void cancelPluginAutomation(AutomationPass pass, session::EffectOwner key);
    std::optional<AutomationPass> m_automationPass;
    QString m_sampleEditLabel;
    std::atomic<bool> m_cancelSampleEdit{false};
    std::optional<SampleEditRequest> m_sampleEditRequest;
    // The effect preview: where the history stood before it, so Apply and Cancel
    // can return there; and the captured effect waiting for its sample edit.
    struct EffectPreview {
        session::TrackId track = session::kInvalidTrack;
        session::Selection selection;
        QString uri;
        std::string name;
        session::InsertIdentity identity{};
        std::size_t cursor = 0;
        std::uint64_t serial = 0;
        bool dirty = false;
        std::uint64_t generation = 0;
    };
    std::optional<EffectPreview> m_effectPreview;
    // Tidy pauses (backend_pauses.cpp).
    struct PauseFind {
        QString scope;
        std::vector<session::TrackId> tracks;
        session::FramePos start = 0, end = 0;
        double minSeconds = 0.5, paddingSeconds = 0.1;
        bool autoThreshold = true, keepBreaths = true;
        std::vector<std::pair<session::FramePos, session::FramePos>> pauses;
        std::vector<bool> kept;
        double thresholdDb = 0, noiseFloorDb = 0;
        QString stamp;
    };
    struct PausePlan {
        QString reason;
        std::vector<std::pair<session::FramePos, session::FramePos>> removals;   // shorten / remove
        std::vector<std::size_t> changed;                                         // pause indices
        std::vector<session::TrackId> moving;
        session::FramePos lengthBefore = 0, removed = 0;
    };
    std::optional<PauseFind> m_pauseFind;
    // Frequency response (backend_eq_curve.cpp): results by settings key, the key
    // being measured, and the newest one asked for while it ran.
    QHash<QString, QVariantMap> m_responses;
    QStringList m_responseOrder;                // oldest first, for trimming
    QString m_responseRunning, m_responseWanted;
    std::thread m_responseThread;
    std::atomic<bool> m_responseCancel{false};
    quint64 m_pluginResponseRevision = 0;
    std::map<std::string, std::vector<lv2::ControlPort>> m_responseProbePorts;
    [[nodiscard]] const std::vector<lv2::ControlPort>* responsePorts(const ChainTarget& target, int slot);
    [[nodiscard]] double responseValue(const ChainTarget& target, int slot, const lv2::ControlPort& port) const;
    [[nodiscard]] QVariantList responseBands(const ChainTarget& target, int slot);
    QVariantMap m_pauseResult;
    PausePlan planPauses(const QVariantMap& action) const;
    // findPauses and applyPauses without their busy checks: a recipe's Shorten
    // pauses step runs them while the recipe holds the project.
    QVariantMap scanPauses(const QVariantMap& spec);
    bool tidyPauses(const QVariantMap& action);
    void runPauseStep(const QVariantMap& step);
    // Recipes (backend_recipes.cpp).
    struct RecipeRunner {
        QString id, name, target, targetText;
        session::TrackId track = session::kInvalidTrack;
        session::FramePos start = 0, end = 0;
        session::Selection selectionBefore;
        session::TrackId focusBefore = session::kInvalidTrack;
        std::uint64_t undoVersion = 0;          // the history before the run
        QVariantList steps;
        int index = -1;                         // the step running
        bool waiting = false, stopping = false;
        std::uint64_t stepVersion = 0;          // the history before this step
        double loudnessBefore = 0;
        QString stepResult;                     // a step that reports its own result
        QElapsedTimer runClock, stepClock;
    };
    std::optional<RecipeRunner> m_recipeRunner;
    QVariantMap m_recipeRunView;
    QTimer* m_recipeTimer = nullptr;
    bool m_recipeApplying = false;            // a recipe's own edits: not recorded, not "Apply Again"
    bool m_recordingSteps = false;
    QVariantList m_recordedSteps;
    QString m_lastRecipeId;
    QVariantList ownRecipes() const;
    void storeOwnRecipes(const QVariantList& list);
    void recordStep(const QVariantMap& step);
    QVariantMap effectStepFor(const session::Insert& insert, const std::string& name);
    void recipeTick();
    void startRecipeStep();
    void endRecipeStep(bool ok, const QString& result, const QString& reason);
    void finishRecipe(bool stopped);
    void publishRecipeStep(int index, const QVariantMap& changes);
    bool recipeBusy() const;
    std::optional<session::Insert> recipeInsert(const QString& uri, const std::map<std::string, double>& bySymbol, QString& error);
    std::optional<session::Insert> soundFixInsert(const QString& id, double amount, double mains, QString& error);
    QString recipeStepInsert(const QVariantMap& step, session::Insert& insert, QString& name);
    struct LastEffect { session::Insert insert; std::string name; };
    std::optional<LastEffect> m_lastEffect;
    bool m_effectPreviewApplying = false;
    struct StagedEffect {
        std::shared_ptr<lv2::HostFeatures> features;   // outlives the chain
        std::shared_ptr<lv2::Chain> chain;
        std::vector<session::Insert> inserts;
        std::string name;
    };
    std::optional<StagedEffect> m_stagedEffect;
    void rollBackEffectPreview(std::function<void(const EffectPreview&)> then);
    void startEffectApply(const EffectPreview& preview, session::Insert insert);
    std::deque<SampleEditJob> m_pendingSampleEdit;
    bool m_shuttingDown = false;
    bool m_graphicsCheckpoint = false;
    bool m_resetStreaming = true;
    [[nodiscard]] std::size_t playbackCacheBytes() const;
    void scheduleMemoryRelease();
    int m_systemMemoryMiB = 4096;
    int m_memoryMiB = 0; // zero = automatic
    QTimer *m_memoryReleaseTimer = nullptr;
    bool m_lastElasticPreparing = false, m_lastElasticFailed = false;
    qulonglong m_lastStreamMisses = 0, m_lastStreamReadErrors = 0;
    bool m_storageActive = false;
    std::optional<StorageRequest> m_storageRequest;
    std::deque<StorageJob> m_pendingStorage;
    std::atomic<bool> m_cancelStorage{false};
    QString m_storageMessage, m_storagePreviewStamp;
    QVariantList m_unusedAudioFiles;
    std::vector<io::UnusedAudioFile> m_cleanupPreview;
    std::uint64_t m_sessionGeneration = 1;
    std::uint64_t m_editRevision = 0;
    // Observations invalidate stale jobs, but only confirmed edits dirty a Save.
    std::uint64_t m_confirmedEditRevision = 0, m_savedRevision = 0;
    // AAF can merge captured live settings, but cannot overwrite a newer host
    // timeline/control/automation edit with its worker-owned project candidate.
    std::uint64_t m_hostEditRevision = 0;
    qulonglong m_documentChangeRequest = 0;

    session::Selection m_selection;
    session::UndoStack m_undo;
    std::vector<session::Clip> m_clipboard;
    session::FramePos m_clipboardFrames = 0;
    int m_clipboardChannel = -1;
    std::optional<session::MultitrackClipboard> m_multitrackClipboard;
    std::vector<session::TrackId> m_selectedTracks;
    std::vector<session::ClipAddress> m_selectedClips;
    bool m_clipGroupsLinked = true;
    session::TrackId m_trackSelectionAnchor = session::kInvalidTrack;
    std::uint64_t m_trackSelectionRevision = 0;

    QVariantList m_markers;
    bool m_snapping = true;
    QString m_snapGrid = QStringLiteral("none");
    qint64 m_snapTolerance = 0;
    bool m_ripple = false;
    double m_masterGainDb = 0.0;
    bool m_metronome = false;
    TrackIdModel m_trackIdModel;
    std::unordered_map<int, QVariantMap> m_publishedTracks;
    int m_tracksRevision = 0;
    QVariantList m_trackSummaries;
    mutable std::uint64_t m_lastLoadSamples = 0;
    bool m_checkingComputer = false;
    QVariantMap m_computerCheck;
    std::shared_ptr<std::atomic<bool>> m_computerCheckCancel = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t m_liveScopeWritten = 0;
    bool m_aligningSpeech = false;
    QString m_speechAlignMessage;
    std::uint64_t m_speechAlignSerial = 0;
    std::shared_ptr<std::atomic<bool>> m_speechAlignCancel;
    bool m_remixing = false;
    QString m_remixMessage;
    std::uint64_t m_remixSerial = 0;
    std::shared_ptr<std::atomic<bool>> m_remixCancel;
    double m_metronomeLevelDb = -12.0;
    QVariantList m_trackMeters;
    int m_meterRevision = 0;
    std::unordered_map<int, std::pair<double, double>> m_meterById;
    std::vector<float> m_lastMeters;

    bool m_dirty = false;
    bool m_saving = false;
    bool m_saveActive = false;
    std::deque<QString> m_saveRequests;
    std::optional<engine::Transport::ControlAccess> m_snapshotAccess;
    QTimer* m_snapshotRetryTimer = nullptr;
    bool m_autosavePending = false;
    QTimer *m_autosaveTimer = nullptr;
    int m_autosaveSeconds = 30;
    bool m_autosaving = false;
    QString m_lastAutosave;
    QString m_instanceId;               // names this run's index file
    QString m_scratchBundle;            // this instance's private recovery bundle
    QString m_recoveryWrittenTo;        // the bundle this run last autosaved into
    std::optional<RecoveryEntry> m_recoveryOffer;
    QString m_projectPath;

    QFileSystemWatcher *m_keymapWatcher = nullptr;

    // Published on the GUI thread; an earlier borrowed world is retained above.
    std::unique_ptr<lv2::World> m_lv2 = std::make_unique<lv2::World>();
    lv2::UridMap m_urids;
    std::vector<std::unique_ptr<lv2::HostFeatures>> m_oldRateFeatures;
    std::unique_ptr<lv2::HostFeatures> m_features;
    std::unordered_map<session::EffectOwner, std::shared_ptr<lv2::Chain>> m_chains;
    std::vector<std::shared_ptr<lv2::Chain>> m_retiredChains;
    struct OpenEditor {
        session::EffectOwner key{};
        int slot = 0;
        session::InsertIdentity identity{};
        std::weak_ptr<lv2::Chain> chain;
        host::Instance* processor = nullptr;           // the live processor shown
        std::unique_ptr<host::EmbeddedEditor> adapter; // an LV2 interface; outlives its window
        std::unique_ptr<PluginEditorWindow> window;
    };
    std::vector<OpenEditor> m_editors;
    // Closes editors whose processor is no longer the live one (or all of them)
    // while that processor still exists. Called before retired chains are freed.
    void pruneEditors(bool all = false);
    // Keeps worker-owned offline instances alive until joinWorker on the GUI.
    std::vector<std::shared_ptr<lv2::Chain>> m_offlineChains;
    std::shared_ptr<lv2::HostFeatures> m_offlineFeatures;
    std::deque<DeliverySnapshot> m_deliverySnapshots;
    std::optional<engine::Transport::ControlAccess> m_deliveryAccess;
    QTimer* m_deliveryTimer = nullptr;
    bool m_pumpingDeliverySnapshots = false;
    std::unique_ptr<FileBatchProcessor> m_fileBatch;
    std::thread m_collectThread;
    std::atomic<bool> m_cancelCollect{false};
    std::atomic<std::size_t> m_collectPhase{0};
    std::atomic<float> m_collectProgress{0};
    bool m_collecting = false;
    QTimer* m_collectTimer = nullptr;
    QVariantMap m_collectionStatus;
    // Instantiation settings only; automation stays in the project/render plans.
    std::unordered_map<session::EffectOwner, std::vector<session::Insert>> m_chainSpecs;

    QVariantList m_plugins;
    QVariantMap m_effectMenu;
    PluginLibrary m_pluginLibrary{stateDir() + QStringLiteral("/plugin-library.json")};
    PluginPresets m_pluginPresets{stateDir() + QStringLiteral("/plugin-parameters.json")};
    // Presets (backend_plugin_presets.cpp): an effect's own LV2 presets, read once
    // per effect; and one preset resolved to every control, ready to apply.
    QHash<QString, QVariantList> m_ownPluginPresets;
    struct ChosenPreset {
        QString name;
        QMap<QString, double> controls;     // symbol (or CLAP id) → value
        bool parameterIds = false;
        QStringList states;
    };
    QVariantList presetFields(const session::Insert& insert, QString& reason);
    void listPresets(const QString& uri, const QVariantList& fields, QVariantMap& out);
    bool choosePreset(const QString& id, const QString& uri, const QVariantList& fields, ChosenPreset& out);
    RecentDocuments m_recentDocuments{stateDir() + QStringLiteral("/recent-documents.json")};
    QString m_recentDocumentsError;
    void rememberDocument(const QString &path, bool project);
    bool m_pluginsListed = false;
    bool m_scanning = false;
    // Staged application integration. Enable with defaultVst3ScanOptions() only
    // after the remaining state/restart/lifecycle acceptance in vst3-hosting-plan.
    std::optional<vst3::CatalogueOptions> m_vst3ScanOptions;
    bool m_vst3Discovery = false;
    bool m_pluginCatalogueComplete = false;
    std::atomic<bool> m_cancelPluginScan{false};
    QStringList m_pluginScanWarnings;
    QVariantList m_pluginPorts;
    QVariantList m_pluginOutputs;
    QString m_pluginPortsKind;
    QString m_pluginPortsUri;
    quint64 m_pluginValuesRevision = 0;
    quint64 m_pluginTextRevision = 0;
    void refreshPluginValues();
    int m_pluginPortsSlot = -1;
    void followPluginPorts(int target, int from, int to);
    [[nodiscard]] bool finishedJobsWaiting();
    void enqueueAudioCommand(const engine::Command &command);
    void flushPendingCommands();
    void flushStoppedCommands();
    void honourPluginRestarts();
    void pollPluginNotifications();
    void consumePluginNotifications();
    struct NativeInsertHistory {
        std::optional<session::Insert> before;
        std::optional<std::vector<session::PluginState>> verifiedState;
        lv2::Chain::InputValues verifiedInputs;
        std::vector<std::pair<std::size_t, std::uint32_t>> gestures;
        struct HostControl { std::uint32_t port; double value; std::uint64_t revision, settingsRevision; };
        std::vector<HostControl> hostControls;
        std::uint64_t revision = 0, hostRevision = 0, nativeRevision = 0;
        std::uint64_t readoutRevision = 0, readoutNativeRevision = 0;
        qint64 readoutAt = 0;
        bool readoutPending = false;
        bool dirty = false;
        bool baselineKnown = false;
        bool resyncPending = false;
    };
    struct NativeChainHistory {
        std::weak_ptr<lv2::Chain> processor;
        std::vector<NativeInsertHistory> inserts;
        std::uint64_t generation = 0, revision = 0, attempted = 0;
        bool capturing = false;
        qint64 retryAt = 0;
        unsigned failures = 0;
    };
    std::unordered_map<session::EffectOwner, std::shared_ptr<NativeChainHistory>> m_nativeHistory;
    std::uint64_t m_pluginSettingSequence = 0;
    const std::vector<session::Insert>* chainInserts(session::EffectOwner key);
    std::shared_ptr<NativeChainHistory> preparePluginHistory(const std::shared_ptr<lv2::Chain>& chain,
        std::vector<session::Insert>& inserts, bool requireKnown);
    void establishPluginHistory(session::EffectOwner key, const std::shared_ptr<lv2::Chain>& chain);
    void noteNativeParameter(session::EffectOwner key, std::size_t slot, std::size_t actual,
                             const host::ParameterEvent& event);
    void noteNativeState(session::EffectOwner key, std::size_t slot);
    void noteNativeReadout(session::EffectOwner key, std::size_t slot);
    bool hasUnverifiedPluginState() const;
    void verifyReadoutState(session::EffectOwner key, const std::shared_ptr<lv2::Chain>& chain,
                            std::size_t slot, const session::Insert& captured);
    std::optional<std::vector<session::PluginState>> normalizeReadoutInputs(
        const session::Insert& captured, const std::vector<session::PluginState>& baseline,
        const lv2::Chain::InputValues& previous, const lv2::Chain::InputValues& current);
    void noteNativeOverflow(session::EffectOwner key, std::size_t slot);
    void pumpNativeHistory();
    void installNativeHistory(session::EffectOwner key, const std::shared_ptr<NativeChainHistory>& runtime,
                              const DeliverySnapshot& snapshot, const std::vector<std::uint64_t>& hostRevisions,
                              bool finishGesture = false);
    std::optional<bool> deferPluginEdit(std::function<bool()> edit, std::function<void()> abandoned = {},
                                        bool protectHistory = false, std::function<void()> owned = {});
    void advanceQueuedHistory(std::uint64_t from);
    void cancelPluginEdits();
    void pumpPluginEdits();
    int pluginSlot(session::EffectOwner key, const session::InsertIdentity& identity);
    struct PluginEdit {
        std::function<bool()> apply;
        std::function<void()> abandoned;
        std::optional<std::uint64_t> historyVersion;
        std::function<void()> owned;
    };
    std::deque<PluginEdit> m_pluginEdits;
    quint64 m_pluginEditSequence = 0;
    bool m_applyingPluginEdit = false, m_pluginEditCapturing = false;
    bool m_pollingPluginNotifications = false;
    quint64 m_chainRebuilds = 0;
    std::unordered_map<session::EffectOwner, qint64> m_chainRestartedAt;
    QTimer *m_commandRetryTimer = nullptr;
    int m_pluginPortsTarget = -1;
    bool m_pluginPortsComplete = false;

    // --- the master bus and the Mastering page ---
    QVariantList m_masterInserts;
    qint64 m_masterLatency = 0;
    QVariantMap m_loudness;
    bool m_previewBefore = false;
    bool m_hearWithoutEffects = false;
    double m_previewMatchDb = 0.0;
    bool m_previewMatched = false;
    bool m_analysing = false;
    bool m_measuringTrackLoudness = false;
    bool m_trackLoudnessStale = true;
    double m_trackLoudnessProgress = 0;
    QString m_trackLoudnessMessage;
    TrackLoudnessJob m_trackLoudness;
    std::deque<TrackLoudnessJob> m_pendingTrackLoudness;
    std::atomic<bool> m_cancelTrackLoudness{false};
    bool m_generatingDucking = false, m_duckingReady = false, m_duckingStale = true;
    double m_duckingProgress = 0;
    QString m_duckingMessage;
    std::uint64_t m_duckingSerial = 0, m_duckingRunningSerial = 0;
    DuckingJob m_ducking;
    std::deque<DuckingJob> m_pendingDucking;
    std::atomic<bool> m_cancelDucking{false};
    bool m_analysisStale = true;
    // Plan/parameter publications can invalidate audio without a document edit.
    // Readout uncertainty suspends use but does not advance this hard boundary.
    std::uint64_t m_analysisEpoch = 0;
    bool m_analysisValidityQueued = false;
    Analysis m_analysis;
    std::deque<Analysis> m_pendingAnalysis;
    std::uint32_t m_masterPeakSequence = 0;
    QVariantList m_masterLevels;
    QVariantMap m_masterReport;
    QVariantList m_shortTermHistory;
    QVariantList m_momentaryHistory;
    std::vector<RenderJob> m_renders;
    int m_nextRenderId = 1;
    int m_activeRender = -1;
    double m_renderProgress = 0.0;
    std::deque<RenderOutcome> m_pendingRender;
    std::atomic<bool> m_cancelLoad{false};
    QVariantMap m_audioSetup;
    QString m_importPlacement = QStringLiteral("start");
    // The markers and bext a render writes for a spec, in export-rate frames.
    session::AudioMetadata exportMetadata(const QString &markers, std::uint32_t exportRate) const;
    AudioCommand *m_audioSetupScan = nullptr;
    AudioLinks *m_outputLinks = nullptr;
    QString m_audioLinkMessage;
    bool startOutputLinks(int nodeId, bool disconnectPorts);

    // The reference picture.
    void syncVideoReference();
    void finishVideoProbe(quint64 generation, const video::VideoInfo &info);
    void videoTick();
    [[nodiscard]] double videoSeconds(qint64 playhead) const;
    [[nodiscard]] bool commitVideo(const char *label, const session::VideoReference &next);
    std::string m_videoProbedPath;
    quint64 m_videoGeneration = 0;
    std::thread m_videoProbeThread;
    std::atomic<bool> m_videoProbeCancel{false};
    bool m_videoProbing = false;
    video::VideoInfo m_videoInfo;
    std::unique_ptr<video::Decoder> m_videoDecoder;
    QImage m_videoFrame;
    double m_videoFrameSeconds = -1.0;
    QTimer *m_videoTimer = nullptr;
    int m_videoWatchers = 0;
    QString m_videoTimecode;
    mutable QVariantMap m_manual;
    std::atomic<bool> m_cancelJob{false};
    std::atomic<float> m_jobProgress{0.0f};

    // --- the Spectral page ---
    spectral::StftConfig m_stft;
    int m_overlap = 4;
    QVariantMap m_spectral;
    bool m_spectralBuilding = false;
    bool m_spectralStale = true;
    bool m_spectralWanted = false;
    double m_spectralProgress = 0.0;
    session::TrackId m_spectralTrack = session::kInvalidTrack;
    std::uint32_t m_spectralSequence = 0;
    spectral::SpectralSet m_spectralSet;
    DisplayCache::Lease m_spectralCache;
    std::deque<SpectralResult> m_pendingSpectral;
    QTimer *m_spectralTimer = nullptr;
    double m_bandLowHz = 0.0;
    double m_bandHighHz = 0.0;
    bool m_repairing = false;
    std::atomic<bool> m_cancelRepair{false};
    QString m_repairSummary, m_repairChannel = QStringLiteral("all");
    QString m_spectralView = QStringLiteral("separate");
    std::optional<RepairRequest> m_queuedRepair;
    std::vector<spectral::Click> m_clicks;
    QVariantList m_clickList;
    QString m_clickSummary;
    bool m_detecting = false;
    std::atomic<bool> m_cancelDetect{false};
    bool m_autoHealPending = false;
    std::optional<double> m_queuedDetect;     // a detect asked for while the slot was busy
    std::deque<DetectResult> m_pendingDetect;
    std::deque<RepairResult> m_pendingRepair;
    std::optional<NoisePrintRequest> m_noisePrintRequest;
    std::deque<NoisePrintJob> m_pendingNoisePrint;
    std::uint64_t m_noisePrintSequence = 0;
    bool m_noisePrintActive = false;
    std::atomic<bool> m_cancelNoisePrint{false};
    spectral::NoisePrint m_noisePrint;
    QString m_noisePrintSummary;
    std::map<session::TrackId, std::map<std::string, double>> m_soundChoices;
    spectral::MaskShapes m_mask;
    QVariantList m_maskShapes;
    int m_maskFeather = 2;
    bool m_pitchTracking = false, m_pitchActive = false;
    std::uint64_t m_maskRevision = 0;
    std::atomic<bool> m_cancelPitch{false};
    std::optional<PitchRequest> m_pitchRequest;
    std::deque<PitchResult> m_pendingPitch;

    void maintainSpectralStore(bool clear);
    QString m_timeDisplay = QStringLiteral("time");
    session::MusicalTime m_displayMusicalTime;
    [[nodiscard]] omaaudio::timeformat::Display currentTimeDisplay() const;
    SpectralStore m_spectralStore;
    int m_spectralStoreGiB = 4;
    std::atomic<int> m_spectralStoreMinimumMs{250};
    std::atomic<quint64> m_spectralStoreHits{0};
    qint64 m_spectralStoreBytes = -1, m_spectralStoreFree = -1;
    int m_spectralStoreEntries = 0;
    bool m_spectralStoreBusy = false, m_spectralStoreAgain = false, m_spectralStoreClearPending = false;
    QString m_spectralStoreStatus;
    // One maintenance task at a time. Declared last: it is joined before
    // anything a task might still be reading is destroyed.
    QThreadPool m_storePool;
};
