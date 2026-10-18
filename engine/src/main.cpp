// Omawave engine.
//
// BRIEF.md amendment A3: the UI is a QML plugin in another process and talks to this
// binary over a unix socket. Nothing here links Qt, which is what makes section 3's
// "Session must not depend on Qt" structural rather than a matter of discipline.

#include "engine/commands.h"
#include "engine/meter_channel.h"
#include "engine/pw_output.h"
#include "engine/render_plan.h"
#include "engine/transport.h"
#include "io/audio_file.h"
#include "io/peak_cache.h"
#include "io/peak_pyramid.h"
#include "io/peak_texture.h"
#include "io/project_file.h"
#include "io/resampler.h"
#include "io/timeline_peaks.h"
#include "ipc/protocol.h"
#include "ipc/server.h"
#include "lv2/chain.h"
#include "lv2/host_features.h"
#include "lv2/instance.h"
#include "lv2/urid.h"
#include "lv2/world.h"
#include "session/command.h"
#include "session/edits.h"
#include "session/clip_envelope.h"
#include "session/project.h"
#include "session/selection.h"
#include "session/state.h"
#include "session/track.h"
#include "session/zero_crossing.h"

#include <pipewire/pipewire.h>

#include <csignal>
#include <sys/prctl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace omawave;

constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultQuantum = 256;      // proven xrun-free in spike A
constexpr std::uint32_t kOutputChannels = 2;
constexpr std::uint32_t kSamplesPerPeak = 256;
constexpr int kStatusIntervalMs = 16;               // ~60Hz playhead

std::string basename(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Where generated peak textures live. Runtime dir rather than the project, because no
// phase has claimed section 4's project file yet.
std::string runtimeDir()
{
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && *xdg != '\0')
        return std::string(xdg) + "/omaaudio";
    if (const char* tmp = std::getenv("TMPDIR"); tmp != nullptr && *tmp != '\0')
        return std::string(tmp) + "/omaaudio";
    return "/tmp/omaaudio";
}

// Result of decoding a file, handed from the worker back to the loop thread.
struct LoadedTake {
    bool ok = false;
    bool replaceProject = false;        // "open" replaces, "import" adds a track
    std::string error;
    std::string path;
    std::string name;

    session::AudioBuffer audio;
    std::uint32_t channels = 0;
    std::uint32_t originalRate = 0;
    bool resampled = false;
};

// Result of saving or loading a project, handed from the worker back to the loop
// thread. Both are slow enough to need one: the first save of a 30-minute take writes
// hundreds of megabytes.
struct SaveJob {
    bool ok = false;
    std::string error;
    std::string path;
    std::uint64_t revision = 0;
};

struct LoadJob {
    io::LoadResult result;
    std::string path;
};

// Peaks for one track's clip list, rebuilt after every edit to that track.
struct PeakSet {
    bool ok = false;
    session::TrackId track = session::kInvalidTrack;
    io::PeakPyramid pyramid;
    std::vector<io::PeakTexture> textures;
};

class App {
    friend struct LegacyAppTest;
public:
    App(std::uint32_t rate, std::uint32_t quantum)
        : project_(rate), transport_(commands_, meters_, rate),
          output_(transport_, rate, kOutputChannels, quantum)
    {
    }

    ~App() { shutdown(); }

    [[nodiscard]] bool start(const std::string& socketPath, std::string& error)
    {
        if (!server_.listen(socketPath, error))
            return false;
        if (!output_.start(error))
            return false;
        outputStarted_ = true;

        server_.onMessage([this](const ipc::Message& m) { handle(m); });
        server_.onConnect([this] { sendHello(); });
        server_.onWake([this] { drainWorker(); });
        server_.onTick([this] { pushStatus(); }, kStatusIntervalMs);

        std::printf("[engine] listening on %s\n", socketPath.c_str());
        std::fflush(stdout);
        return true;
    }

    void run(int timeoutSeconds) { server_.run(timeoutSeconds); }

    void shutdown()
    {
        discardWorker();
        output_.stop();
        outputStarted_ = false;
        flushStoppedCommands();
        collectRetiredPlans();
        for (const auto& entry : peaks_)
            removeTextures(entry.second);
        peaks_.clear();
    }

private:
    // --- message handling, loop thread -------------------------------------------

    void handle(const ipc::Message& m)
    {
        const std::string& type = m.type();

        if (type == "open" || type == "import") {
            const std::optional<std::string> path = m.string("path");
            if (!path.has_value()) {
                sendError(type + " requires a path");
                return;
            }
            beginLoad(*path, type == "open");
        } else if (type == "play") {
            sendCommand(engine::Command::play());
        } else if (type == "stop") {
            sendCommand(engine::Command::stop());
        } else if (type == "seek") {
            if (const std::optional<std::int64_t> frame = m.integer("frame"))
                sendCommand(engine::Command::seek(*frame));
        } else if (type == "gain") {
            if (const std::optional<double> db = m.number("db")) {
                // dB converts here rather than on the audio thread, so the callback
                // never runs a std::pow.
                sendCommand(engine::Command::setGain(linearFromDb(*db)));
            }
        } else if (type == "select") {
            handleSelect(m);
        } else if (type == "selectAll") {
            selection_ = session::Selection{
                .start = 0, .end = focusedLength(), .track = focused_};
            sendSelection(false);
        } else if (type == "clearSelection") {
            selection_ = session::Selection{
                .start = 0, .end = 0, .track = focused_};
            sendSelection(false);
        } else if (type == "save") {
            beginSave(m.string("path").value_or(projectPath_));
        } else if (type == "load") {
            if (const std::optional<std::string> path = m.string("path"))
                beginLoad(*path, true, /*isProject=*/true);
            else
                sendError("load requires a path");
        } else if (type == "listPlugins") {
            beginScan();
        } else if (type == "addPlugin") {
            handleAddPlugin(m);
        } else if (type == "removePlugin") {
            handleRemovePlugin(m);
        } else if (type == "movePlugin") {
            handleMovePlugin(m);
        } else if (type == "setPluginBypass") {
            handleSetPluginBypass(m);
        } else if (type == "setPluginControl") {
            handleSetPluginControl(m);
        } else if (type == "pluginPorts") {
            handlePluginPorts(m);
        } else if (type == "newProject") {
            newProject();
        } else if (type == "quit") {
            server_.stop();
        } else if (!handleTrack(m)) {
            handleEdit(m);
        }
    }

    static float linearFromDb(double db) noexcept
    {
        return static_cast<float>(std::pow(10.0, db / 20.0));
    }

    static double dbFromLinear(float linear) noexcept
    {
        // -inf is not a number the wire format can carry, and a fader has to stop
        // somewhere. -120 dB is far below anything audible.
        if (linear <= 1e-6f)
            return -120.0;
        return 20.0 * std::log10(static_cast<double>(linear));
    }

    // --- tracks -------------------------------------------------------------------

    // Returns true when the message was a track operation, handled or rejected.
    bool handleTrack(const ipc::Message& m)
    {
        const std::string& type = m.type();

        if (type == "addTrack") {
            const session::SessionState before = session::captureState(project_);
            const session::TrackId id =
                project_.addTrack(m.string("name").value_or(nextTrackName()));
            focus(id);
            commit("add track", id, before);
            return true;
        }

        if (type == "removeTrack") {
            const std::optional<session::TrackId> id = trackArg(m);
            if (!id.has_value())
                return true;
            const session::SessionState before = session::captureState(project_);
            const std::optional<std::size_t> index = project_.trackIndex(*id);
            if (!project_.removeTrack(*id))
                return true;
            rebuildChain(*id);
            forgetPeaks(*id);
            // Focus the track that took its place, or the one above it if it was last.
            if (focused_ == *id) {
                const std::vector<session::Track>& tracks = project_.tracks();
                if (tracks.empty())
                    focus(session::kInvalidTrack);
                else
                    focus(tracks[std::min(index.value_or(0), tracks.size() - 1)].id);
            }
            commit("remove track", *id, before);
            return true;
        }

        if (type == "renameTrack") {
            const std::optional<session::TrackId> id = trackArg(m);
            const std::optional<std::string> name = m.string("name");
            if (!id.has_value() || !name.has_value())
                return true;
            session::Track* track = project_.track(*id);
            if (track == nullptr)
                return true;
            const session::SessionState before = session::captureState(project_);
            track->name = *name;
            commit("rename track", *id, before);
            return true;
        }

        if (type == "moveTrack") {
            const std::optional<session::TrackId> id = trackArg(m);
            const std::optional<std::int64_t> to = m.integer("index");
            if (!id.has_value() || !to.has_value())
                return true;
            const session::SessionState before = session::captureState(project_);
            if (project_.moveTrack(*id, static_cast<std::size_t>(std::max<std::int64_t>(0, *to))))
                commit("move track", *id, before);
            return true;
        }

        if (type == "focusTrack") {
            if (const std::optional<session::TrackId> id = trackArg(m)) {
                focus(*id);
                sendTracks();
                sendSelection(false);
            }
            return true;
        }

        if (type == "setTrackGain" || type == "setTrackPan" || type == "setTrackMute"
            || type == "setTrackSolo") {
            handleTrackParam(m);
            return true;
        }

        return false;
    }

    // Gain, pan, mute and solo do not rebuild the plan. They are gestures a user makes
    // continuously -- dragging a fader, tapping solo -- and republishing a plan per
    // gesture would churn allocations for a change that touches four floats. They also
    // stay out of the undo stack: undoing a fader move is not what Ctrl+Z is for, and
    // it would bury real edits under a hundred entries per drag.
    void handleTrackParam(const ipc::Message& m)
    {
        const std::optional<session::TrackId> id = trackArg(m);
        if (!id.has_value())
            return;
        session::Track* track = project_.track(*id);
        if (track == nullptr)
            return;

        const std::string& type = m.type();
        if (type == "setTrackGain") {
            if (const std::optional<double> db = m.number("db"))
                track->gain = linearFromDb(*db);
        } else if (type == "setTrackPan") {
            if (const std::optional<double> pan = m.number("pan"))
                track->pan = std::clamp(static_cast<float>(*pan), -1.0f, 1.0f);
        } else if (type == "setTrackMute") {
            track->muted = m.boolean("on").value_or(!track->muted);
        } else if (type == "setTrackSolo") {
            track->soloed = m.boolean("on").value_or(!track->soloed);
        }

        markDirty();

        // Solo changes what every other track sounds like, so all of them are pushed,
        // not just the one that was touched.
        publishTrackParams();
        sendTrackParams();
    }

    void publishTrackParams()
    {
        const bool anySolo = project_.anySoloed();
        for (const session::Track& t : project_.tracks()) {
            sendCommand(engine::Command::setTrackParams(
                t.id, t.effectiveGain(anySolo), session::panGainsFor(t.pan)));
        }
    }

    [[nodiscard]] std::optional<session::TrackId> trackArg(const ipc::Message& m) const
    {
        if (const std::optional<std::int64_t> raw = m.integer("track")) {
            if (*raw >= 0)
                return static_cast<session::TrackId>(static_cast<std::uint32_t>(*raw));
        }
        if (focused_ != session::kInvalidTrack)
            return focused_;
        return std::nullopt;
    }

    // The bundle's directory name without its extension, which is what a title bar
    // wants. Empty when the session has never been saved.
    [[nodiscard]] std::string projectName() const
    {
        if (projectPath_.empty())
            return {};
        std::string name = basename(projectPath_);
        // .omaaudio, or .omawave for a project saved before the rename.
        for (const std::string suffix : {".omaaudio", ".omawave"})
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                name.resize(name.size() - suffix.size());
                break;
            }
        return name;
    }

    [[nodiscard]] std::string nextTrackName() const
    {
        return "Track " + std::to_string(project_.tracks().size() + 1);
    }

    void focus(session::TrackId id)
    {
        if (focused_ == id)
            return;
        focused_ = id;
        // The range survives a focus change, clamped to the new track. Losing it would
        // make "apply the same trim to the other take" a re-drag every time.
        clampSelection();
    }

    [[nodiscard]] std::vector<session::Clip>* focusedClips() noexcept
    {
        session::Track* track = project_.track(focused_);
        return track == nullptr ? nullptr : &track->clips;
    }

    [[nodiscard]] session::FramePos focusedLength() const noexcept
    {
        const session::Track* track = project_.track(focused_);
        return track == nullptr ? 0 : track->length();
    }

    // --- edits --------------------------------------------------------------------

    // Installs a state, republishes, and marks every track's peaks dirty. Used by undo
    // and redo, which can change any number of tracks at once.
    void installState(const session::SessionState& state)
    {
        session::restoreState(project_, state);

        // Undo can add a track back, remove one, or change a chain, and the snapshot
        // carries insert lists. Rebuilding every chain here is what makes undoing
        // "add a plugin" actually stop the plugin running.
        rebuildAllChains();

        if (project_.track(focused_) == nullptr) {
            focused_ = project_.tracks().empty() ? session::kInvalidTrack
                                                 : project_.tracks().front().id;
        }
        publishPlan();
        publishTrackParams();
        clampSelection();
        for (const session::Track& t : project_.tracks())
            markPeaksDirty(t.id);
        pumpPeaks();
    }

    // The one path every clip-list edit takes, so undo can never be forgotten.
    void commit(std::string label, session::TrackId track,
                const session::SessionState& before)
    {
        if (const auto error=session::validateClipEnvelopes(project_); !error.empty()) {
            session::restoreState(project_,before); sendError(error); return;
        }
        session::SessionState after = session::captureState(project_);
        if (!undo_.push(std::move(label), track, before, std::move(after))) {
            // Nothing actually changed -- a split on a clip boundary, 0 dB of gain.
            sendTimeline();
            return;
        }
        markDirty();
        publishPlan();
        publishTrackParams();
        clampSelection();
        markPeaksDirty(track);
        pumpPeaks();
        sendTimeline();
    }

    // Set by everything that changes what a save would write, and by nothing else --
    // seeking, playing and changing focus leave a project clean, because they are not
    // things anyone would be sorry to lose.
    void markDirty()
    {
        ++revision_;
        if (dirty_)
            return;
        dirty_ = true;
        sendTimeline();
    }

    void handleEdit(const ipc::Message& m)
    {
        const std::string& type = m.type();

        if (type == "undo" || type == "redo") {
            const session::EditCommand* command =
                type == "undo" ? undo_.undo() : undo_.redo();
            if (command != nullptr) {
                installState(type == "undo" ? command->before : command->after);
                if (project_.track(command->track) != nullptr)
                    focus(command->track);
                markDirty();
            }
            sendTimeline();
            return;
        }

        std::vector<session::Clip>* clips = focusedClips();
        if (clips == nullptr || clips->empty()) {
            // Not an error worth surfacing: an edit with nothing open is a no-op.
            return;
        }

        const session::SessionState before = session::captureState(project_);
        const session::Selection sel = selection_;
        std::string label;

        if (type == "split") {
            const auto frame = m.integer("frame");
            label = "split";
            session::edits::splitAt(*clips, frame.value_or(selection_.start));
        } else if (type == "delete" || type == "cut") {
            if (sel.empty()) {
                sendError(type + " needs a selection");
                return;
            }
            if (type == "cut")
                clipboard_ = session::edits::copyRange(*clips, sel.start, sel.end);
            label = type;
            session::edits::removeRange(*clips, sel.start, sel.end);
            // The selection no longer refers to anything: its content is gone.
            selection_.end = selection_.start = sel.start;
        } else if (type == "copy") {
            if (sel.empty()) {
                sendError("copy needs a selection");
                return;
            }
            clipboard_ = session::edits::copyRange(*clips, sel.start, sel.end);
            sendTimeline();          // clipboard state changed, nothing else did
            return;
        } else if (type == "paste") {
            if (clipboard_.empty()) {
                sendError("nothing to paste");
                return;
            }
            label = "paste";
            const auto at = m.integer("frame").value_or(meters_.position());
            session::edits::insertAt(*clips, clipboard_, at);
        } else if (type == "trim") {
            if (sel.empty()) {
                sendError("trim needs a selection");
                return;
            }
            label = "trim";
            session::edits::trimToRange(*clips, sel.start, sel.end);
            selection_.end = selection_.start = 0;
        } else if (type == "gainRange") {
            if (sel.empty()) {
                sendError("gain needs a selection");
                return;
            }
            const std::optional<double> db = m.number("db");
            if (!db.has_value()) {
                sendError("gainRange requires db");
                return;
            }
            label = "gain";
            session::edits::applyGain(*clips, sel.start, sel.end, linearFromDb(*db));
        } else if (type == "fadeIn" || type == "fadeOut") {
            if (sel.empty()) {
                sendError(type + " needs a selection");
                return;
            }
            label = type;
            if (type == "fadeIn")
                session::edits::applyFadeIn(*clips, sel.start, sel.end);
            else
                session::edits::applyFadeOut(*clips, sel.start, sel.end);
        } else {
            return;              // not an edit; unknown messages are ignored
        }

        commit(std::move(label), focused_, before);
    }

    void clampSelection()
    {
        if (project_.track(selection_.track) == nullptr)
            selection_.track = focused_;
        selection_ = session::Selection::normalised(selection_.start, selection_.end,
                                                    trackLength(selection_.track),
                                                    selection_.track);
    }

    [[nodiscard]] session::FramePos trackLength(session::TrackId id) const noexcept
    {
        const session::Track* track = project_.track(id);
        return track == nullptr ? 0 : track->length();
    }

    // --- plan publication ---------------------------------------------------------

    // Only the loop thread owns this overflow queue. The callback keeps its fixed
    // SPSC queue and never allocates or waits. Preserve order across plan/chain
    // replacements as well as parameter and transport commands.
    void sendCommand(const engine::Command& command)
    {
        if (pendingCommands_.empty() && commands_.push(command))
            return;
        pendingCommands_.push_back(command);
        flushPendingCommands();
    }

    void flushPendingCommands()
    {
        while (!pendingCommands_.empty() && commands_.push(pendingCommands_.front()))
            pendingCommands_.pop_front();
    }

    // Call only when no output callback is running, before rate changes or teardown.
    void flushStoppedCommands()
    {
        do {
            transport_.flushStopped();
            flushPendingCommands();
        } while (!commands_.empty());
    }

    void publishPlan()
    {
        auto plan = std::make_shared<engine::RenderPlan>(
            engine::buildRenderPlan(project_, kOutputChannels));

        // Chains are attached rather than built here. Rebuilding one means
        // instantiating plugins, and publishing a plan happens on every edit: an
        // instance must outlive a cut, or every keystroke would reload the rack and
        // lose whatever state the plugins had settled into.
        for (engine::PlanTrack& pt : plan->tracks) {
            const auto it = chains_.find(static_cast<std::uint32_t>(pt.id));
            if (it != chains_.end() && it->second != nullptr && !it->second->empty())
                pt.chain = it->second.get();
        }

        ++planGeneration_;
        // The audio thread borrows a raw pointer to this plan. Previous ones are kept
        // alive until the audio thread acknowledges a later generation, so a plan can
        // never be freed while still in use.
        retiring_.push_back(currentPlan_);
        currentPlan_ = std::move(plan);

        sendCommand(
            engine::Command::setPlan(currentPlan_.get(), planGeneration_));

        collectRetiredPlans();
    }

    void collectRetiredPlans()
    {
        // Anything the audio thread has moved past is provably unreferenced -- both the
        // plans and, critically, the audio those plans pointed into.
        if (meters_.acknowledgedPlan() >= planGeneration_) {
            retiring_.clear();
            retiredProjects_.clear();
            retiredChains_.clear();
            retiredFeatures_.clear();
        }
    }

    [[nodiscard]] session::FramePos timelineLength() const noexcept
    {
        return project_.length();
    }

    // --- selection ----------------------------------------------------------------

    void handleSelect(const ipc::Message& m)
    {
        const std::optional<std::int64_t> start = m.integer("start");
        const std::optional<std::int64_t> end = m.integer("end");
        if (!start.has_value() || !end.has_value()) {
            sendError("select requires start and end");
            return;
        }

        // Selecting on a track is also how you focus it: dragging a range on a lane and
        // then finding the edit landed somewhere else would be indefensible.
        if (const std::optional<std::int64_t> raw = m.integer("track")) {
            if (*raw >= 0) {
                const auto id = static_cast<session::TrackId>(
                    static_cast<std::uint32_t>(*raw));
                if (project_.track(id) != nullptr)
                    focused_ = id;
            }
        }

        const bool snap = m.boolean("snap").value_or(true);
        const session::FramePos length = focusedLength();

        session::Selection sel =
            session::Selection::normalised(*start, *end, length, focused_);

        if (snap && !sel.empty()) {
            // Snapping to zero crossings is what stops a cut from clicking. Edges move
            // independently, then the result is re-normalised in case snapping crossed
            // them over, which a very short selection can do.
            const session::FramePos a = snapToZeroCrossing(sel.start);
            const session::FramePos b = snapToZeroCrossing(sel.end);
            sel = session::Selection::normalised(a, b, length, focused_);
        }

        selection_ = sel;
        sendSelection(snap);
    }

    // Timeline positions are not source positions once the take has been edited, so
    // the crossing search has to happen in the clip that covers the frame.
    [[nodiscard]] session::FramePos snapToZeroCrossing(session::FramePos frame) const
    {
        const session::Track* track = project_.track(focused_);
        if (track == nullptr)
            return frame;

        for (const session::Clip& c : track->clips) {
            if (!c.covers(frame))
                continue;
            const session::AudioSource* source = project_.source(c.source);
            if (source == nullptr)
                break;

            const session::FramePos inSource = c.sourceOffset + (frame - c.timelineStart);
            const session::FramePos snapped =
                session::nearestZeroCrossing(source->audio, inSource);
            const session::FramePos backOnTimeline =
                c.timelineStart + (snapped - c.sourceOffset);

            // Refuse to snap across a clip boundary: the neighbouring audio is
            // unrelated, so a crossing there means nothing.
            if (backOnTimeline >= c.timelineStart && backOnTimeline <= c.timelineEnd())
                return backOnTimeline;
            break;
        }
        return frame;
    }

    // --- outbound messages --------------------------------------------------------

    void sendHello()
    {
        ipc::Message m("hello");
        m.set("sampleRate", static_cast<std::int64_t>(project_.sampleRate()));
        m.set("channels", static_cast<std::int64_t>(kOutputChannels));
        m.set("samplesPerPeak", static_cast<std::int64_t>(kSamplesPerPeak));
        m.set("maxTracks", static_cast<std::int64_t>(engine::Transport::kMaxTracks));
        server_.send(m);
    }

    void sendError(const std::string& what)
    {
        ipc::Message m("error");
        m.set("message", what);
        server_.send(m);
    }

    void sendSelection(bool snapped)
    {
        ipc::Message m("selection");
        m.set("start", static_cast<std::int64_t>(selection_.start));
        m.set("end", static_cast<std::int64_t>(selection_.end));
        m.set("length", static_cast<std::int64_t>(selection_.length()));
        m.set("track", trackWire(selection_.track));
        m.set("empty", selection_.empty());
        m.set("snapped", snapped);
        server_.send(m);
    }

    static std::int64_t trackWire(session::TrackId id) noexcept
    {
        return id == session::kInvalidTrack
                 ? -1
                 : static_cast<std::int64_t>(static_cast<std::uint32_t>(id));
    }

    void addPeakLevelsTo(ipc::Message& m, const std::string& prefix,
                         const PeakSet& peaks) const
    {
        m.set(prefix + "levelCount", static_cast<std::int64_t>(peaks.textures.size()));
        for (std::size_t level = 0; level < peaks.textures.size(); ++level) {
            const io::PeakTexture& tex = peaks.textures[level];
            const std::string key = prefix + "level" + std::to_string(level) + ".";
            if (!tex.ok) {
                m.set(key + "error", tex.error);
                continue;
            }
            m.set(key + "texture", tex.path);
            m.set(key + "peakCount", static_cast<std::int64_t>(tex.peakCount));
            m.set(key + "samplesPerPeak",
                  static_cast<std::int64_t>(peaks.pyramid.levels[level].samplesPerPeak));
            m.set(key + "textureWidth", static_cast<std::int64_t>(tex.width));
            m.set(key + "textureHeight", static_cast<std::int64_t>(tex.height));
        }
    }

    // The track list: structure, parameters, and where each lane's peak textures are.
    void sendTracks()
    {
        const bool anySolo = project_.anySoloed();

        ipc::Message m("tracks");
        m.set("trackCount", static_cast<std::int64_t>(project_.tracks().size()));
        m.set("focused", trackWire(focused_));
        m.set("anySolo", anySolo);

        for (std::size_t i = 0; i < project_.tracks().size(); ++i) {
            const session::Track& t = project_.tracks()[i];
            const std::string prefix = "track" + std::to_string(i) + ".";
            m.set(prefix + "id", trackWire(t.id));
            m.set(prefix + "name", t.name);
            m.set(prefix + "gainDb", dbFromLinear(t.gain));
            m.set(prefix + "pan", static_cast<double>(t.pan));
            m.set(prefix + "muted", t.muted);
            m.set(prefix + "soloed", t.soloed);
            m.set(prefix + "silenced", t.effectiveGain(anySolo) == 0.0f);
            m.set(prefix + "frames", static_cast<std::int64_t>(t.length()));
            m.set(prefix + "clipCount", static_cast<std::int64_t>(t.clips.size()));

            // The chain, named but not described: port descriptors are asked for on
            // demand, because `tracks` goes out on every edit and one LSP plugin has
            // 1030 ports.
            m.set(prefix + "insertCount", static_cast<std::int64_t>(t.inserts.size()));
            for (std::size_t j = 0; j < t.inserts.size(); ++j) {
                const session::Insert& insert = t.inserts[j];
                const std::string slot = prefix + "insert" + std::to_string(j) + ".";
                const lv2::PluginInfo* info = activeJob_ == Job::Scan
                    ? nullptr : lv2World_.find(insert.uri);
                m.set(slot + "uri", insert.uri);
                m.set(slot + "name", info != nullptr ? info->name : insert.uri);
                m.set(slot + "bypass", insert.bypass);
                // False for a plugin this box does not have. The slot is kept and
                // reported either way, so the rack can grey it rather than lose it.
                m.set(slot + "available", info != nullptr && info->usable);
            }

            const auto chain = chains_.find(static_cast<std::uint32_t>(t.id));
            m.set(prefix + "latency",
                  static_cast<std::int64_t>(chain != chains_.end() && chain->second
                                                ? chain->second->latencyFrames()
                                                : 0));

            const auto it = peaks_.find(static_cast<std::uint32_t>(t.id));
            if (it != peaks_.end() && it->second.ok)
                addPeakLevelsTo(m, prefix, it->second);
            else
                m.set(prefix + "levelCount", std::int64_t{0});
        }
        server_.send(m);
    }

    // The cheap counterpart to `tracks`: parameters only, for fader and knob moves that
    // change no structure and no waveform.
    void sendTrackParams()
    {
        const bool anySolo = project_.anySoloed();

        ipc::Message m("trackParams");
        m.set("trackCount", static_cast<std::int64_t>(project_.tracks().size()));
        m.set("anySolo", anySolo);
        for (std::size_t i = 0; i < project_.tracks().size(); ++i) {
            const session::Track& t = project_.tracks()[i];
            const std::string prefix = "track" + std::to_string(i) + ".";
            m.set(prefix + "id", trackWire(t.id));
            m.set(prefix + "gainDb", dbFromLinear(t.gain));
            m.set(prefix + "pan", static_cast<double>(t.pan));
            m.set(prefix + "muted", t.muted);
            m.set(prefix + "soloed", t.soloed);
            m.set(prefix + "silenced", t.effectiveGain(anySolo) == 0.0f);
        }
        server_.send(m);
    }

    void sendTimeline()
    {
        const session::FramePos length = timelineLength();

        ipc::Message m("timeline");
        m.set("frames", static_cast<std::int64_t>(length));
        m.set("durationSeconds",
              static_cast<double>(length) / static_cast<double>(project_.sampleRate()));
        m.set("trackCount", static_cast<std::int64_t>(project_.tracks().size()));
        m.set("canUndo", undo_.canUndo());
        m.set("canRedo", undo_.canRedo());
        m.set("undoLabel", undo_.undoLabel());
        m.set("redoLabel", undo_.redoLabel());
        m.set("hasClipboard", !clipboard_.empty());
        m.set("dirty", dirty_);
        m.set("projectPath", projectPath_);
        m.set("projectName", projectName());
        m.set("saving", saving_);
        server_.send(m);

        sendTracks();
        sendSelection(false);
    }

    void pushStatus()
    {
        flushPendingCommands();
        collectRetiredPlans();
        if (!server_.connected())
            return;

        const auto position = meters_.position();
        const bool playing = meters_.playing();

        // Only speak when something changed. A stopped, idle engine should not be
        // waking the UI sixty times a second.
        if (position == lastSentPosition_ && playing == lastSentPlaying_)
            return;
        lastSentPosition_ = position;
        lastSentPlaying_ = playing;

        ipc::Message m("status");
        m.set("frame", static_cast<std::int64_t>(position));
        m.set("playing", playing);
        m.set("peakL", static_cast<double>(meters_.peakLeft()));
        m.set("peakR", static_cast<double>(meters_.peakRight()));
        server_.send(m);
    }

    // --- workers ------------------------------------------------------------------

    enum class Job { None, Load, Peaks, Save, LoadProject, Scan };

    void beginLoad(const std::string& path, bool replaceProject, bool isProject = false)
    {
        if (isProject) {
            beginLoadProject(path);
            return;
        }

        if (busy_.load(std::memory_order_acquire)) {
            sendError("still working on the previous file");
            return;
        }
        joinWorker();
        busy_.store(true, std::memory_order_release);
        activeJob_ = Job::Load;
        workerRevision_ = revision_;

        ipc::Message m("loading");
        m.set("path", path);
        server_.send(m);

        const std::uint32_t projectRate = project_.sampleRate();

        worker_ = std::thread([this, path, projectRate, replaceProject] {
            LoadedTake result = decode(path, projectRate);
            result.replaceProject = replaceProject;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                pendingLoad_ = std::move(result);
                finishedJob_ = Job::Load;
            }
            server_.wake();
        });
    }

    static LoadedTake decode(const std::string& path, std::uint32_t projectRate)
    {
        LoadedTake out;
        out.path = path;
        out.name = basename(path);

        io::AudioFile file = io::readAudioFile(path);
        if (!file.ok) {
            out.error = file.error;
            return out;
        }

        out.originalRate = file.sampleRate;
        session::AudioBuffer audio = std::move(file.audio);
        if (file.sampleRate != projectRate) {
            // Section 2: resample on import, never mid-graph. The only place in the
            // engine that changes sample rate.
            const io::Resampler resampler(file.sampleRate, projectRate);
            audio = resampler.process(audio);
            out.resampled = true;
        }

        out.channels = audio.channels();
        out.audio = std::move(audio);
        out.ok = true;
        return out;
    }

    // --- lv2 catalogue --------------------------------------------------------------
    //
    // Measured on the target box: loading the LV2 world is 10 ms, and walking every
    // port of the 360 plugins installed is 1060 ms. The port walk is what needs the
    // worker -- a second on the loop thread is a second of frozen playhead -- and it is
    // why the catalogue is built once and kept.

    void beginScan()
    {
        if (busy_.load(std::memory_order_acquire)) {
            sendError("still working; ask for the plugin list again in a moment");
            return;
        }
        if (lv2World_.scanned()) {
            sendPlugins();
            return;
        }
        joinWorker();
        busy_.store(true, std::memory_order_release);
        activeJob_ = Job::Scan;

        ipc::Message m("scanning");
        server_.send(m);

        worker_ = std::thread([this] {
            lv2World_.scan();
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                finishedJob_ = Job::Scan;
            }
            server_.wake();
        });
    }

    // --- inserts ------------------------------------------------------------------

    lv2::HostFeatures& hostFeatures()
    {
        if (features_ == nullptr) {
            features_ = std::make_unique<lv2::HostFeatures>(
                urids_, static_cast<double>(project_.sampleRate()), lv2::kMaxBlock);
        }
        return *features_;
    }

    // Builds one track's chain from the model, replacing whatever it had.
    //
    // Whole-chain, not incremental. A chain's slots are wired to alternating scratch
    // buffers, so inserting into the middle of one rewires everything after it anyway;
    // rebuilding is the same work with none of the ways to get the wiring wrong. The
    // cost is that adding a second plugin re-instantiates the first, which is why
    // step 8's saved state matters and why this is not on the path of an ordinary edit.
    //
    // Runs on the main thread, which is not the audio thread: the callback carries on
    // playing the old plan, with the old chain, until the new one is published.
    void rebuildChain(session::TrackId id)
    {
        const session::Track* track = project_.track(id);
        const auto key = static_cast<std::uint32_t>(id);

        // Nothing to do when the model still describes the chain that is running.
        // Undo restores whole snapshots, so without this an undone cut on one track
        // would re-instantiate every plugin on every other one -- and a rack of LSP
        // plugins takes long enough that you would feel it.
        if (const auto built = chainSpecs_.find(key);
            built != chainSpecs_.end() && track != nullptr
            && built->second == track->inserts) {
            return;
        }

        // The outgoing chain may still be running under the current plan, so it is
        // retired rather than dropped.
        if (const auto it = chains_.find(key); it != chains_.end()) {
            if (it->second != nullptr)
                retiredChains_.push_back(std::move(it->second));
            chains_.erase(it);
        }

        chainSpecs_.erase(key);

        if (track == nullptr || track->inserts.empty())
            return;

        if (!lv2World_.scanned())
            lv2World_.scan();

        auto chain = std::make_unique<lv2::Chain>(kOutputChannels, lv2::kMaxBlock);
        for (const session::Insert& insert : track->inserts) {
            std::string error;
            if (!chain->append(lv2World_, hostFeatures(), insert, error)) {
                // The slot stays in the model: a project that names a plugin this box
                // does not have must keep it, values and all, or opening a session on
                // the wrong machine quietly throws away a day's work. An empty live
                // slot preserves subsequent model/control indices.
                chain->appendMissing(insert.identity);
                sendError(error);
            }
        }

        if (chain->empty())
            return;

        // Before anything can hear it. 28 of the plugins installed here allocate during
        // their first blocks, and this is the thread where they are allowed to.
        if (!chain->warmUp(nullptr)) { sendError("Effect warm-up failed."); return; }
        chains_[key] = std::move(chain);
        chainSpecs_[key] = track->inserts;
    }

    void rebuildAllChains()
    {
        for (const session::Track& t : project_.tracks())
            rebuildChain(t.id);

        // Tracks that have gone away entirely -- undoing an import, loading another
        // project -- leave a chain behind that nothing will ever look up again.
        for (auto it = chains_.begin(); it != chains_.end();) {
            if (project_.track(static_cast<session::TrackId>(it->first)) == nullptr) {
                if (it->second != nullptr)
                    retiredChains_.push_back(std::move(it->second));
                chainSpecs_.erase(it->first);
                it = chains_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void handleAddPlugin(const ipc::Message& m)
    {
        if (activeJob_ == Job::Scan) {
            sendError("still scanning; try adding the plugin again in a moment");
            return;
        }
        const std::optional<session::TrackId> id = trackArg(m);
        const std::optional<std::string> uri = m.string("uri");
        if (!id.has_value() || !uri.has_value()) {
            sendError("addPlugin requires a track and a uri");
            return;
        }
        session::Track* track = project_.track(*id);
        if (track == nullptr) {
            sendError("addPlugin: no such track");
            return;
        }

        if (!lv2World_.scanned())
            lv2World_.scan();       // asking for a plugin by name is asking for the scan

        const lv2::PluginInfo* info = lv2World_.find(*uri);
        if (info == nullptr) {
            sendError("addPlugin: plugin not installed: " + *uri);
            return;
        }
        if (!info->usable) {
            sendError("addPlugin: " + info->name + " cannot be an insert: "
                      + info->unusableReason);
            return;
        }

        // Refuse before touching the model, so a refusal is not an undo step.
        if (!info->fits(kOutputChannels) && !info->fits(1)) {
            sendError("addPlugin: " + info->name + " does not fit this track");
            return;
        }

        const session::SessionState before = session::captureState(project_);

        session::Insert insert;
        insert.uri = *uri;

        // Where in the chain. Past the end, or unspecified, means the end of it.
        const auto slot = static_cast<std::size_t>(
            std::max<std::int64_t>(0, m.integer("slot").value_or(
                                          static_cast<std::int64_t>(track->inserts.size()))));
        if (slot >= track->inserts.size())
            track->inserts.push_back(std::move(insert));
        else
            track->inserts.insert(track->inserts.begin() + static_cast<long>(slot),
                                  std::move(insert));

        rebuildChain(*id);
        commitInserts("add " + info->name, *id, before);
    }

    void handleRemovePlugin(const ipc::Message& m)
    {
        const std::optional<session::TrackId> id = trackArg(m);
        if (!id.has_value()) {
            sendError("removePlugin requires a track");
            return;
        }
        session::Track* track = project_.track(*id);
        if (track == nullptr || track->inserts.empty()) {
            sendError("removePlugin: nothing to remove");
            return;
        }

        const std::optional<std::int64_t> raw = m.integer("slot");
        if (!raw.has_value() || *raw < 0
            || static_cast<std::size_t>(*raw) >= track->inserts.size()) {
            sendError("removePlugin: no such slot");
            return;
        }

        const session::SessionState before = session::captureState(project_);
        track->inserts.erase(track->inserts.begin() + static_cast<long>(*raw));

        rebuildChain(*id);
        commitInserts("remove plugin", *id, before);
    }

    // A slot's live instance, or null. Both control moves and bypass need it, and
    // neither should be pushing a command at a chain that is not running.
    [[nodiscard]] std::pair<session::Track*, std::size_t>
    insertArg(const ipc::Message& m, const char* what)
    {
        const std::optional<session::TrackId> id = trackArg(m);
        if (!id.has_value()) {
            sendError(std::string(what) + " requires a track");
            return {nullptr, 0};
        }
        session::Track* track = project_.track(*id);
        const std::optional<std::int64_t> slot = m.integer("slot");
        if (track == nullptr || !slot.has_value() || *slot < 0
            || static_cast<std::size_t>(*slot) >= track->inserts.size()) {
            sendError(std::string(what) + ": no such slot");
            return {nullptr, 0};
        }
        return {track, static_cast<std::size_t>(*slot)};
    }

    void handleSetPluginControl(const ipc::Message& m)
    {
        const auto [track, slot] = insertArg(m, "setPluginControl");
        if (track == nullptr)
            return;

        const std::optional<std::int64_t> port = m.integer("port");
        const std::optional<double> value = m.number("value");
        if (!port.has_value() || *port < 0 || *port > std::numeric_limits<std::uint32_t>::max()
            || !value.has_value() || !std::isfinite(*value)) {
            sendError("setPluginControl requires a port and a value");
            return;
        }

        const auto index = static_cast<std::uint32_t>(*port);

        // The model is authoritative -- it is what a save reads, and it is never read
        // back off the running instance. The command is a copy in flight.
        track->inserts[slot].setControl(index, *value);
        const auto chain = chains_.find(static_cast<std::uint32_t>(track->id));
        const auto parameterVersion = chain != chains_.end() && chain->second
            ? chain->second->parameterVersion(slot) : 0;
        sendCommand(
            engine::Command::setPluginControl(session::EffectOwner::track(track->id), slot, index, *value, parameterVersion));

        // Dirty but not undoable, exactly as a fader move is: burying real edits under
        // a hundred entries per knob drag is not what Ctrl+Z is for.
        markDirty();
    }

    void handleSetPluginBypass(const ipc::Message& m)
    {
        const auto [track, slot] = insertArg(m, "setPluginBypass");
        if (track == nullptr)
            return;

        const session::SessionState before = session::captureState(project_);
        const bool on = m.boolean("on").value_or(!track->inserts[slot].bypass);
        track->inserts[slot].bypass = on;

        // Straight at the audio thread rather than through a rebuild: a bypass toggle
        // must not reload the plugin, both because it would drop a block and because
        // whatever the plugin had settled into would be gone.
        sendCommand(engine::Command::setPluginBypass(session::EffectOwner::track(track->id), slot, on));

        // The rebuild guard reads chainSpecs_, so the record of what is running has to
        // follow the model or the next unrelated edit would rebuild the rack.
        const auto key = static_cast<std::uint32_t>(track->id);
        if (const auto built = chainSpecs_.find(key); built != chainSpecs_.end()
            && slot < built->second.size()) {
            built->second[slot].bypass = on;
        }

        // Membership -- and bypass is membership -- is undoable.
        commitInserts(on ? "bypass plugin" : "unbypass plugin", track->id, before);
    }

    void handleMovePlugin(const ipc::Message& m)
    {
        const std::optional<session::TrackId> id = trackArg(m);
        const std::optional<std::int64_t> from = m.integer("from");
        const std::optional<std::int64_t> to = m.integer("to");
        if (!id.has_value() || !from.has_value() || !to.has_value()) {
            sendError("movePlugin requires a track, a from and a to");
            return;
        }
        session::Track* track = project_.track(*id);
        if (track == nullptr || *from < 0
            || static_cast<std::size_t>(*from) >= track->inserts.size()) {
            sendError("movePlugin: no such slot");
            return;
        }

        const auto size = static_cast<std::int64_t>(track->inserts.size());
        const auto target = static_cast<std::size_t>(std::clamp(*to, std::int64_t{0},
                                                                size - 1));
        const auto source = static_cast<std::size_t>(*from);
        if (source == target)
            return;

        const session::SessionState before = session::captureState(project_);
        session::Insert moved = track->inserts[source];
        track->inserts.erase(track->inserts.begin() + static_cast<long>(source));
        track->inserts.insert(track->inserts.begin() + static_cast<long>(target),
                              std::move(moved));

        rebuildChain(*id);
        commitInserts("reorder plugins", *id, before);
    }

    // Port descriptors, on demand and chunked.
    //
    // Not sent with `tracks`: that goes out on every edit, and one LSP plugin here has
    // 1030 control ports. Chunked because the same plugin's descriptors do not fit in
    // kMaxMessageBytes.
    void handlePluginPorts(const ipc::Message& m)
    {
        if (activeJob_ == Job::Scan) {
            sendError("still scanning; ask for plugin controls again in a moment");
            return;
        }
        const auto [track, slot] = insertArg(m, "pluginPorts");
        if (track == nullptr)
            return;

        const session::Insert& insert = track->inserts[slot];
        if (!lv2World_.scanned())
            lv2World_.scan();

        std::string error;
        std::unique_ptr<lv2::Instance> probe =
            lv2::Instance::create(lv2World_, insert.uri, hostFeatures(), error);
        if (probe == nullptr) {
            sendError("pluginPorts: " + error);
            return;
        }

        // Only the knobs. Output controls are meters and readouts; drawing them as
        // controls would offer to set something the plugin owns.
        std::vector<const lv2::ControlPort*> inputs;
        for (const lv2::ControlPort& port : probe->controls()) {
            if (!port.output)
                inputs.push_back(&port);
        }

        constexpr std::size_t kPerMessage = 96;
        const std::size_t total = inputs.size();
        for (std::size_t first = 0; first < total || first == 0; first += kPerMessage) {
            const std::size_t count = std::min(kPerMessage, total - std::min(first, total));

            ipc::Message out("pluginPortsInfo");
            out.set("track", trackWire(track->id));
            out.set("slot", static_cast<std::int64_t>(slot));
            out.set("uri", insert.uri);
            out.set("first", static_cast<std::int64_t>(first));
            out.set("count", static_cast<std::int64_t>(count));
            out.set("total", static_cast<std::int64_t>(total));

            for (std::size_t i = 0; i < count; ++i) {
                const lv2::ControlPort& port = *inputs[first + i];
                const std::string prefix = "port" + std::to_string(i) + ".";
                out.set(prefix + "index", static_cast<std::int64_t>(port.index));
                out.set(prefix + "symbol", port.symbol);
                out.set(prefix + "name", port.name);
                out.set(prefix + "min", static_cast<double>(port.minimum));
                out.set(prefix + "max", static_cast<double>(port.maximum));
                out.set(prefix + "default", static_cast<double>(port.defaultValue));
                out.set(prefix + "toggled", port.toggled);
                out.set(prefix + "integer", port.integer);

                // What the model holds, which is what a save would write -- not what
                // the running instance happens to have in its buffer.
                const session::InsertControl* stored = insert.control(port.index);
                out.set(prefix + "value",
                        static_cast<double>(stored != nullptr ? stored->value
                                                             : port.defaultValue));
            }
            server_.send(out);

            if (total == 0)
                break;
        }
    }

    // Chain membership is part of SessionState, so it is undoable for free -- but it
    // changes no clip and no audio, so it does not dirty the peaks the way an edit
    // does. This is commit() without the peak rebuild.
    void commitInserts(std::string label, session::TrackId track,
                       const session::SessionState& before)
    {
        session::SessionState after = session::captureState(project_);
        if (!undo_.push(std::move(label), track, before, std::move(after))) {
            sendTimeline();
            return;
        }
        markDirty();
        publishPlan();
        sendTracks();
        sendTimeline();
    }

    static const char* layoutWire(lv2::Layout layout)
    {
        switch (layout) {
        case lv2::Layout::Mono:         return "mono";
        case lv2::Layout::Stereo:       return "stereo";
        case lv2::Layout::MonoOrStereo: return "either";
        case lv2::Layout::Unsupported:  break;
        }
        return "none";
    }

    void sendPlugins()
    {
        const std::vector<lv2::PluginInfo>& found = lv2World_.plugins();

        // One message, not chunked: the whole catalogue is around a hundred kilobytes
        // where kMaxMessageBytes is a megabyte. Port descriptors are the thing that
        // will need chunking -- one LSP plugin has 1030 control ports on its own.
        ipc::Message m("plugins");
        m.set("pluginCount", static_cast<std::int64_t>(found.size()));

        for (std::size_t i = 0; i < found.size(); ++i) {
            const lv2::PluginInfo& p = found[i];
            const std::string prefix = "plugin" + std::to_string(i) + ".";
            m.set(prefix + "uri", p.uri);
            m.set(prefix + "name", p.name);
            m.set(prefix + "class", p.className);
            m.set(prefix + "author", p.author);
            m.set(prefix + "layout", std::string(layoutWire(p.layout)));
            m.set(prefix + "inMin", static_cast<std::int64_t>(p.requiredIn));
            m.set(prefix + "inMax", static_cast<std::int64_t>(p.totalIn));
            m.set(prefix + "outMin", static_cast<std::int64_t>(p.requiredOut));
            m.set(prefix + "outMax", static_cast<std::int64_t>(p.totalOut));
            m.set(prefix + "sidechain", static_cast<std::int64_t>(p.sidechainIn));
            m.set(prefix + "controls", static_cast<std::int64_t>(p.controlIn));
            m.set(prefix + "usable", p.usable);
            if (!p.usable)
                m.set(prefix + "reason", p.unusableReason);
        }
        server_.send(m);
    }

    void markPeaksDirty(session::TrackId track)
    {
        if (track == session::kInvalidTrack)
            return;
        if (std::find(dirtyPeaks_.begin(), dirtyPeaks_.end(), track) == dirtyPeaks_.end())
            dirtyPeaks_.push_back(track);
    }

    // Starts the next dirty track's rebuild if the single worker slot is free. Peaks
    // are rebuilt one track at a time and only for tracks that actually changed: a
    // rebuild walks a whole take and writes a seven-level pyramid, and doing that for
    // every track on every keystroke would undo the entire point of the peak cache.
    void pumpPeaks()
    {
        if (dirtyPeaks_.empty() || busy_.load(std::memory_order_acquire))
            return;

        const session::TrackId track = dirtyPeaks_.front();
        dirtyPeaks_.pop_front();

        const session::Track* source = project_.track(track);
        if (source == nullptr) {
            pumpPeaks();             // removed under us; try the next one
            return;
        }

        joinWorker();
        busy_.store(true, std::memory_order_release);
        activeJob_ = Job::Peaks;

        const std::vector<session::Clip> snapshot = source->clips;
        const std::uint32_t sequence = ++peakSequence_;

        worker_ = std::thread([this, snapshot, sequence, track] {
            // Reads project_ for its audio sources only. Sources are never added or
            // removed while a worker runs -- installTake joins first -- and clip lists
            // are copied into `snapshot` rather than read from here.
            PeakSet result = buildPeaks(project_, snapshot, sequence, track);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                pendingPeaks_ = std::move(result);
                finishedJob_ = Job::Peaks;
            }
            server_.wake();
        });
    }

    static PeakSet buildPeaks(const session::Project& project,
                              const std::vector<session::Clip>& clips,
                              std::uint32_t sequence, session::TrackId track)
    {
        PeakSet out;
        out.track = track;

        io::PeakLevel base = io::buildTimelinePeaks(project, clips, kSamplesPerPeak,
                                                    kOutputChannels);
        if (base.empty()) {
            // An empty track is a valid state, not a failure: its lane draws nothing.
            out.ok = true;
            return out;
        }

        out.pyramid = io::pyramidFromLevel(std::move(base));

        std::error_code ec;
        std::filesystem::create_directories(runtimeDir(), ec);

        const auto rawTrack = static_cast<std::uint32_t>(track);
        out.textures.reserve(out.pyramid.size());
        for (std::size_t level = 0; level < out.pyramid.size(); ++level) {
            // Filenames carry the track and a sequence number so Qt's image cache
            // cannot serve the previous version's texture after an edit.
            const std::string path = runtimeDir() + "/peaks-" + std::to_string(rawTrack)
                                   + "-" + std::to_string(sequence)
                                   + "-" + std::to_string(level) + ".png";
            out.textures.push_back(
                io::writePeakTexture(out.pyramid.levels[level], path));
        }

        out.ok = true;
        return out;
    }

    void joinWorker()
    {
        if (worker_.joinable())
            worker_.join();
    }

    // Completion still owns the worker slot until the loop consumes or discards
    // it. Joining alone is insufficient: a queued wake must not install an old
    // take, save path or texture after a session replacement.
    void discardWorker()
    {
        joinWorker();
        removeTextures(pendingPeaks_);
        pendingPeaks_ = {};
        pendingLoad_ = {};
        pendingSave_ = {};
        pendingLoadProject_ = {};
        finishedJob_ = Job::None;
        activeJob_ = Job::None;
        busy_.store(false, std::memory_order_release);
        saving_ = false;
    }

    void drainWorker()
    {
        Job job = Job::None;
        LoadedTake take;
        PeakSet peaks;
        SaveJob save;
        LoadJob loadProject;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            job = finishedJob_;
            finishedJob_ = Job::None;
            if (job == Job::Load)
                take = std::move(pendingLoad_);
            else if (job == Job::Peaks)
                peaks = std::move(pendingPeaks_);
            else if (job == Job::Save)
                save = std::move(pendingSave_);
            else if (job == Job::LoadProject)
                loadProject = std::move(pendingLoadProject_);
        }

        if (job == Job::None)
            return;
        joinWorker();
        activeJob_ = Job::None;
        busy_.store(false, std::memory_order_release);

        // Replacing a session must not erase an edit made while decoding. Adding
        // a track can merge into the current session; New already discards jobs.
        const bool replaces = job == Job::LoadProject
            || (job == Job::Load && take.replaceProject);
        if (replaces && workerRevision_ != revision_) {
            sendError("project changed while opening; open the file again to replace it");
            pumpPeaks();
            return;
        }

        if (job == Job::Load) {
            if (!take.ok) {
                sendError(take.error.empty() ? "could not open file" : take.error);
            } else
                installTake(std::move(take));
        } else if (job == Job::Peaks) {
            installPeaks(std::move(peaks));
        } else if (job == Job::Save) {
            finishSave(std::move(save));
        } else if (job == Job::LoadProject) {
            finishLoadProject(std::move(loadProject));
        } else if (job == Job::Scan) {
            sendPlugins();
        }

        pumpPeaks();
    }

    // --- projects -----------------------------------------------------------------

    void beginSave(const std::string& path)
    {
        if (path.empty()) {
            // Not an error: the UI asks for a path and sends `save` again. Saying so
            // rather than failing is what lets Ctrl+S mean "save" on a fresh session.
            ipc::Message m("needsPath");
            server_.send(m);
            return;
        }
        if (busy_.load(std::memory_order_acquire)) {
            sendError("still working; try saving again in a moment");
            return;
        }
        joinWorker();
        busy_.store(true, std::memory_order_release);
        activeJob_ = Job::Save;
        saving_ = true;
        sendTimeline();

        // The project and the view are copied for the worker rather than referenced.
        // A save takes seconds on a long take, and the user carries on editing while it
        // runs -- a reference would be a race against their next keystroke.
        const session::Project snapshot = project_;
        io::ProjectView view;
        view.focus = focused_;
        view.selection = selection_;
        const std::uint64_t revision = revision_;

        worker_ = std::thread([this, snapshot, view, path, revision] {
            const io::SaveResult saved = io::saveProject(snapshot, view, path);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                pendingSave_ = SaveJob{.ok = saved.ok, .error = saved.error,
                                      .path = path, .revision = revision};
                finishedJob_ = Job::Save;
            }
            server_.wake();
        });
    }

    void finishSave(SaveJob job)
    {
        saving_ = false;
        if (!job.ok) {
            sendError(job.error.empty() ? "could not save the project" : job.error);
            sendTimeline();
            return;
        }

        projectPath_ = job.path;
        dirty_ = job.revision != revision_;

        ipc::Message m("saved");
        m.set("path", projectPath_);
        m.set("name", projectName());
        server_.send(m);
        sendTimeline();
    }

    void beginLoadProject(const std::string& path)
    {
        if (busy_.load(std::memory_order_acquire)) {
            sendError("still working on the previous file");
            return;
        }
        joinWorker();
        busy_.store(true, std::memory_order_release);
        activeJob_ = Job::LoadProject;
        workerRevision_ = revision_;

        ipc::Message m("loading");
        m.set("path", path);
        server_.send(m);

        worker_ = std::thread([this, path] {
            LoadJob job;
            job.path = path;
            job.result = io::loadProject(path);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                pendingLoadProject_ = std::move(job);
                finishedJob_ = Job::LoadProject;
            }
            server_.wake();
        });
    }

    void finishLoadProject(LoadJob job)
    {
        if (!job.result.ok || !job.result.project.has_value()) {
            // The current session is untouched: loadProject builds a whole new Project
            // and only hands it over once it is complete.
            sendError(job.result.error.empty() ? "could not open the project"
                                               : job.result.error);
            return;
        }

        const auto rate = job.result.project->sampleRate();
        if (rate < 8000 || rate > 192000) {
            sendError("project sample rate is not supported by the output");
            return;
        }
        discardWorker();
        const bool rateChanged = rate != output_.sampleRate();
        const bool restartOutput = rateChanged && outputStarted_;
        if (rateChanged) {
            output_.stop();
            outputStarted_ = false;
            flushStoppedCommands();
            // The rate was validated above and the stream has stopped.
            if (!output_.setSampleRateStopped(rate)) {
                sendError("could not configure the output sample rate");
                return;
            }
            transport_.configureStopped(rate);
        }
        retireProject();
        project_ = std::move(*job.result.project);
        if (features_ && features_->sampleRate() != static_cast<double>(rate))
            retiredFeatures_.push_back(std::move(features_));

        undo_.clear();
        clipboard_.clear();
        focused_ = job.result.view.focus;
        selection_ = job.result.view.selection;
        projectPath_ = job.path;
        dirty_ = false;

        rebuildAllChains();
        publishPlan();
        publishTrackParams();
        if (rateChanged) {
            flushStoppedCommands();
            collectRetiredPlans();
            if (restartOutput) {
                std::string error;
                outputStarted_ = output_.start(error);
                if (!outputStarted_)
                    sendError("project loaded, but output could not restart: " + error);
            }
        }
        clampSelection();

        // A cached peak level is the track's timeline peak level only when the track is
        // an untouched copy of its source. Anything edited is rebuilt, which is the
        // normal path anyway.
        for (const session::Track& track : project_.tracks()) {
            bool installed = false;
            if (io::isWholeSourceClip(project_, track.clips)) {
                const session::AudioSource* source =
                    project_.source(track.clips.front().source);
                if (source != nullptr) {
                    const auto it = job.result.peaks.find(source->uuid.toString());
                    if (it != job.result.peaks.end())
                        installed = installCachedPeaks(track.id, it->second);
                }
            }
            if (!installed)
                markPeaksDirty(track.id);
        }
        pumpPeaks();

        ipc::Message m("loaded");
        m.set("path", job.path);
        m.set("name", projectName());
        m.set("track", trackWire(focused_));
        m.set("sampleRate", static_cast<std::int64_t>(project_.sampleRate()));
        m.set("channels", static_cast<std::int64_t>(kOutputChannels));
        m.set("replaced", true);
        m.set("isProject", true);
        server_.send(m);

        sendTimeline();
        lastSentPosition_ = -1;
    }

    // Turns a cached level-0 peak level into the pyramid and textures a lane draws
    // from, skipping the walk over the take that would otherwise happen on every open.
    [[nodiscard]] bool installCachedPeaks(session::TrackId track, const io::PeakLevel& base)
    {
        if (base.empty() || base.samplesPerPeak != kSamplesPerPeak)
            return false;

        PeakSet set;
        set.track = track;
        set.pyramid = io::pyramidFromLevel(base);

        std::error_code ec;
        std::filesystem::create_directories(runtimeDir(), ec);

        const auto rawTrack = static_cast<std::uint32_t>(track);
        const std::uint32_t sequence = ++peakSequence_;
        set.textures.reserve(set.pyramid.size());
        for (std::size_t level = 0; level < set.pyramid.size(); ++level) {
            const std::string path = runtimeDir() + "/peaks-" + std::to_string(rawTrack)
                                   + "-" + std::to_string(sequence)
                                   + "-" + std::to_string(level) + ".png";
            set.textures.push_back(io::writePeakTexture(set.pyramid.levels[level], path));
        }
        set.ok = true;

        installPeaks(std::move(set));
        return true;
    }

    // The old project's audio is still being read by the audio thread through the
    // current plan, so it is retired rather than destroyed -- freed only once the audio
    // thread acknowledges a plan that no longer refers to it. Moving a Project moves its
    // deque, which keeps every buffer's address.
    void retireProject()
    {
        ++revision_;
        const std::uint32_t rate = project_.sampleRate();
        retiredProjects_.push_back(std::move(project_));
        project_ = session::Project(rate);
        for (auto& entry : chains_)
            if (entry.second)
                retiredChains_.push_back(std::move(entry.second));
        chains_.clear();
        chainSpecs_.clear();

        for (const auto& entry : peaks_)
            removeTextures(entry.second);
        peaks_.clear();
        dirtyPeaks_.clear();
    }

    void newProject()
    {
        discardWorker();
        retireProject();

        undo_.clear();
        clipboard_.clear();
        focused_ = session::kInvalidTrack;
        selection_ = session::Selection{};
        projectPath_.clear();
        dirty_ = false;

        publishPlan();
        publishTrackParams();
        sendTimeline();
        lastSentPosition_ = -1;
    }

    void installTake(LoadedTake take)
    {
        // Adding a source touches the deque the audio thread reads through the plan, so
        // no worker may be running while it happens.
        discardWorker();

        const std::uint32_t originalRate = take.originalRate;
        const bool resampled = take.resampled;
        const std::string path = take.path;
        const std::string name = take.name;
        const std::uint32_t channels = take.channels;
        const auto frames = static_cast<session::FramePos>(take.audio.frames());

        if (take.replaceProject) {
            retireProject();
            undo_.clear();
            clipboard_.clear();
            focused_ = session::kInvalidTrack;
            selection_ = session::Selection{};
            // A take opened over the top of a project is a new, unsaved session rather
            // than an edit of the one that was open.
            projectPath_.clear();
        }
        ++revision_;
        dirty_ = true;

        const session::SourceId id = project_.addSource(
            take.name, take.path, std::move(take.audio),
            resampled ? std::optional<std::uint32_t>(originalRate) : std::nullopt);

        const session::TrackId trackId = project_.addTrack(name);
        session::Track* track = project_.track(trackId);

        session::Clip whole;
        whole.source = id;
        whole.timelineStart = 0;
        whole.sourceOffset = 0;
        whole.length = frames;
        track->clips = {whole};

        focused_ = trackId;
        selection_ = session::Selection{.start = 0, .end = 0, .track = trackId};

        // Importing is undoable; opening is not, because it discards the project the
        // history describes.
        if (!take.replaceProject) {
            session::SessionState before = session::captureState(project_);
            // The state before the import is the current one minus the new track.
            before.tracks.pop_back();
            (void)undo_.push("import", trackId, std::move(before),
                             session::captureState(project_));
        }

        publishPlan();
        publishTrackParams();
        markPeaksDirty(trackId);
        pumpPeaks();

        ipc::Message m("loaded");
        m.set("path", path);
        m.set("name", name);
        m.set("track", trackWire(trackId));
        m.set("frames", static_cast<std::int64_t>(frames));
        m.set("channels", static_cast<std::int64_t>(channels));
        m.set("sampleRate", static_cast<std::int64_t>(project_.sampleRate()));
        m.set("originalSampleRate", static_cast<std::int64_t>(originalRate));
        m.set("resampled", resampled);
        m.set("replaced", take.replaceProject);
        m.set("durationSeconds",
              static_cast<double>(frames) / static_cast<double>(project_.sampleRate()));
        server_.send(m);

        sendTimeline();
        lastSentPosition_ = -1;   // force the next status through
    }

    static void removeTextures(const PeakSet& peaks)
    {
        for (const io::PeakTexture& tex : peaks.textures) {
            if (!tex.ok)
                continue;
            std::error_code ec;
            std::filesystem::remove(tex.path, ec);
        }
    }

    void forgetPeaks(session::TrackId track)
    {
        const auto it = peaks_.find(static_cast<std::uint32_t>(track));
        if (it == peaks_.end())
            return;
        removeTextures(it->second);
        peaks_.erase(it);
    }

    void installPeaks(PeakSet peaks)
    {
        if (!peaks.ok)
            return;
        if (project_.track(peaks.track) == nullptr) {
            // The track was removed while its peaks were being built.
            removeTextures(peaks);
            return;
        }

        const auto key = static_cast<std::uint32_t>(peaks.track);
        const auto it = peaks_.find(key);
        if (it != peaks_.end()) {
            // Previous textures are unreferenced once the new message has been sent.
            removeTextures(it->second);
            it->second = std::move(peaks);
        } else {
            peaks_.emplace(key, std::move(peaks));
        }

        sendTracks();
    }

    // --- state ---------------------------------------------------------------------

    session::Project project_;
    session::TrackId focused_ = session::kInvalidTrack;
    session::UndoStack undo_;
    std::vector<session::Clip> clipboard_;
    session::Selection selection_;

    engine::Transport::Queue commands_;
    std::deque<engine::Command> pendingCommands_;
    engine::MeterChannel meters_;
    engine::Transport transport_;
    engine::PwOutput output_;
    bool outputStarted_ = false;
    ipc::Server server_;

    std::shared_ptr<engine::RenderPlan> currentPlan_;
    std::vector<std::shared_ptr<engine::RenderPlan>> retiring_;
    std::vector<session::Project> retiredProjects_;
    std::uint32_t planGeneration_ = 0;

    lv2::World lv2World_;

    // One URID map for the process, not one per instance: a saved plugin state is
    // written in terms of URIDs, so two plugins have to agree on the number for a
    // string or a state saved by one is nonsense to the other.
    lv2::UridMap urids_;
    std::unique_ptr<lv2::HostFeatures> features_;
    // Must outlive every retired chain which borrowed its LV2 feature pointers.
    std::vector<std::unique_ptr<lv2::HostFeatures>> retiredFeatures_;

    // One chain per track that has inserts. Absent means "no inserts", which is the
    // case the audio thread takes its original, untouched path for.
    std::unordered_map<std::uint32_t, std::unique_ptr<lv2::Chain>> chains_;

    // A chain the audio thread may still be running through the current plan. Freed
    // only once it has acknowledged a plan that no longer refers to it -- the same
    // rule as a replaced Project, and for the same reason.
    std::vector<std::unique_ptr<lv2::Chain>> retiredChains_;

    // What each running chain was built from, so a rebuild can be skipped when the
    // model has not actually changed.
    std::unordered_map<std::uint32_t, std::vector<session::Insert>> chainSpecs_;

    std::unordered_map<std::uint32_t, PeakSet> peaks_;
    std::deque<session::TrackId> dirtyPeaks_;
    std::uint32_t peakSequence_ = 0;

    std::thread worker_;
    std::atomic<bool> busy_{false};
    Job activeJob_ = Job::None; // loop-owned, including completed-but-unconsumed work
    std::uint64_t revision_ = 0;
    std::uint64_t workerRevision_ = 0;
    std::mutex mutex_;
    Job finishedJob_ = Job::None;
    LoadedTake pendingLoad_;
    PeakSet pendingPeaks_;

    std::string projectPath_;
    bool dirty_ = false;
    bool saving_ = false;
    SaveJob pendingSave_;
    LoadJob pendingLoadProject_;

    session::FramePos lastSentPosition_ = -1;
    bool lastSentPlaying_ = false;
};

} // namespace

int main(int argc, char** argv)
{
    std::string socketPath;
    std::uint32_t rate = kDefaultRate;
    std::uint32_t quantum = kDefaultQuantum;
    int timeoutSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "-socket" && i + 1 < argc)
            socketPath = argv[++i];
        else if (arg == "-rate" && i + 1 < argc)
            rate = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (arg == "-quantum" && i + 1 < argc)
            quantum = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (arg == "-seconds" && i + 1 < argc)
            timeoutSeconds = std::atoi(argv[++i]);
        else if (arg == "-help" || arg == "--help") {
            std::printf("usage: omawave-engine -socket <path> [-rate N] "
                        "[-quantum N] [-seconds N]\n");
            return 0;
        }
    }

    if (socketPath.empty()) {
        std::fprintf(stderr, "omawave-engine: -socket <path> is required\n");
        return 1;
    }

    // Die with whoever spawned us. The engine holds a PipeWire stream, and a shell
    // restart otherwise leaves the old one alive: two engines then hold two streams
    // over the same device, and the orphan answers nothing because the new engine has
    // already unlinked and rebound the socket. Observed after an `omarchy-restart-shell`
    // on 2026-08-25.
    //
    // Harmless when there is no parent to speak of -- run from a terminal, the signal
    // arrives when that terminal goes, which is what a person would expect anyway.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    pw_init(&argc, &argv);

    App app(rate, quantum);
    std::string error;
    if (!app.start(socketPath, error)) {
        std::fprintf(stderr, "omawave-engine: %s\n", error.c_str());
        return 1;
    }

    app.run(timeoutSeconds);
    app.shutdown();
    return 0;
}
