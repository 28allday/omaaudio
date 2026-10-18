#include "state.h"
#include "effect_racks.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace omawave::session {

bool SessionState::operator==(const SessionState& other) const
{
    if (tracks != other.tracks || markers != other.markers || masterInserts != other.masterInserts
        || masterVolumeAutomation != other.masterVolumeAutomation || sampleRate != other.sampleRate
        || defaultExportBits != other.defaultExportBits || metadata != other.metadata || video != other.video
        || musicalTime != other.musicalTime || clipGroups != other.clipGroups) return false;
    // Equal rack values do not make independently processed regions equivalent
    // to a shared split run. Compare the alias graph without comparing transient
    // numeric owner tokens, so a separately loaded equivalent graph is equal.
    for (std::size_t t = 0; t < tracks.size(); ++t) for (std::size_t c = 0; c < tracks[t].clips.size(); ++c) {
        const auto& a = tracks[t].clips[c]; const auto& b = other.tracks[t].clips[c];
        if (!a.rack) continue; // persistent clip equality already checks presence
        for (std::size_t priorTrack = 0; priorTrack <= t; ++priorTrack) {
            const auto limit = priorTrack == t ? c : tracks[priorTrack].clips.size();
            for (std::size_t priorClip = 0; priorClip < limit; ++priorClip) {
                const auto& previousA = tracks[priorTrack].clips[priorClip];
                const auto& previousB = other.tracks[priorTrack].clips[priorClip];
                if (previousA.rack && ((previousA.rack->owner == a.rack->owner)
                    != (previousB.rack->owner == b.rack->owner))) return false;
            }
        }
    }
    return true;
}

bool SessionState::sameInsertInstances(const SessionState& other) const noexcept
{
    const auto same = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].identity != b[i].identity) return false;
        return true;
    };
    if (tracks.size() != other.tracks.size() || !same(masterInserts, other.masterInserts)) return false;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id != other.tracks[i].id || !same(tracks[i].inserts, other.tracks[i].inserts)
            || tracks[i].clips.size() != other.tracks[i].clips.size()) return false;
        for (std::size_t c = 0; c < tracks[i].clips.size(); ++c) {
            const auto& a = tracks[i].clips[c]; const auto& b = other.tracks[i].clips[c];
            if (a.identity != b.identity || bool(a.rack) != bool(b.rack)) return false;
            if (a.rack && (a.rack->owner != b.rack->owner || !same(a.rack->inserts, b.rack->inserts))) return false;
        }
    }
    return true;
}

FramePos SessionState::length() const noexcept
{
    FramePos longest = 0;
    for (const TrackState& t : tracks) {
        FramePos end = 0;
        for (const Clip& c : t.clips) {
            if (c.timelineEnd() > end)
                end = c.timelineEnd();
        }
        if (end > longest)
            longest = end;
    }
    return longest;
}

SessionState captureState(const Project& project)
{
    SessionState state;
    state.tracks.reserve(project.tracks().size());
    for (const Track& t : project.tracks()) {
        // Every field, in declaration order: GCC's -Wmaybe-uninitialized sees the
        // assign-then-move form as a partially written struct and refuses it under
        // -Werror once the struct grew past a vector register.
        state.tracks.push_back(TrackState{
            .id = t.id,
            .name = t.name,
            .role = t.role,
            .colour = t.colour,
            .gain = t.gain,
            .pan = t.pan,
            .muted = t.muted,
            .soloed = t.soloed,
            .volumeAutomation = t.volumeAutomation,
            .panAutomation = t.panAutomation,
            .clips = t.clips,
            .inserts = t.inserts,
            .routing = t.routing,
            .ducking = t.ducking,
        });
    }
    state.markers = project.markers();
    state.masterInserts = project.masterInserts();
    state.masterVolumeAutomation = project.masterVolumeAutomation();
    state.sampleRate = project.sampleRate();
    state.defaultExportBits = project.defaultExportBits();
    state.metadata = project.metadata();
    state.video = project.video();
    state.musicalTime = project.musicalTime();
    state.clipGroups = project.clipGroups();
    return state;
}

SessionState stateForPluginAwareEdit(const SessionState& current, const SessionState& from,
                                      const SessionState& to)
{
    auto result = to;
    const auto find = [](const SessionState& state, const Insert& target) -> const Insert* {
        const Insert* found = nullptr;
        forEachEffectRack(state, [&](EffectOwner, const auto& inserts) {
            if (found) return;
            for (const auto& insert : inserts)
                if (insert.identity == target.identity && insert.uri == target.uri) { found = &insert; return; }
        });
        return found;
    };
    const auto merge = [&](Insert& target) {
        const auto* previous = find(from, target);
        const auto* live = find(current, target);
        if (!previous || !live) return; // added, removed or replaced by this edit
        const auto cutoff = std::max(target.settingsRevision, previous->settingsRevision);
        // Rendering a curve can change the captured inputs without constituting
        // a settings edit. When this history step changes that curve, restore its
        // original inputs too (including independently saved mono instances).
        // Truly newer user settings still win through the revision check below.
        const bool automationChanged = std::any_of(previous->controls.begin(), previous->controls.end(), [&](const auto& control) {
            const auto* restored = target.control(control.port);
            return restored ? !restored->sameAutomation(control) : control.hasAutomationData();
        }) || std::any_of(target.controls.begin(), target.controls.end(), [&](const auto& control) {
            return !previous->control(control.port) && control.hasAutomationData();
        });
        // A capture can fold existing explicit overrides into its opaque bytes
        // without being a new user edit. Keep that coherent representation for
        // an unrelated history step. Equal revisions alone are insufficient:
        // older parameter-preset snapshots can have different control values.
        const bool equivalentCapture = !automationChanged && live->settingsRevision == cutoff
            && live->controls.size() == target.controls.size()
            && std::all_of(target.controls.begin(), target.controls.end(), [&](const auto& control) {
                const auto* now = live->control(control.port);
                return now && now->value == control.value && now->revision == control.revision;
            });
        if (target.instanceState == previous->instanceState
            && (live->settingsRevision > cutoff || equivalentCapture)) {
            target.instanceState = live->instanceState;
            target.settingsRevision = live->settingsRevision;
        }
        const bool replacingState = target.instanceState != live->instanceState;
        std::erase_if(target.controls, [&](auto& control) {
            const auto* old = previous->control(control.port);
            const auto* now = live->control(control.port);
            if (!old) return false; // this edit explicitly added the control
            const bool newer = now && (now->revision > std::max(control.revision, old->revision)
                || (equivalentCapture && !replacingState
                    && now->revision == std::max(control.revision, old->revision)));
            if (control == *old && newer) {
                const bool explicitOverride = !control.fromState || control.value != now->value || !now->fromState;
                control = *now;
                if (replacingState && explicitOverride) control.fromState = false;
            } else if (now) {
                if (newer && control.value == old->value && control.fromState == old->fromState) {
                    const bool explicitOverride = !control.fromState || control.value != now->value || !now->fromState;
                    control.value = now->value; control.fromState = now->fromState; control.revision = now->revision;
                    if (replacingState && explicitOverride) control.fromState = false;
                }
                if (control.automation == old->automation) control.automation = now->automation;
                for (std::size_t actual = 0; actual < control.instanceAutomation.size(); ++actual)
                    if (control.instanceAutomation[actual] == old->instanceAutomation[actual])
                        control.instanceAutomation[actual] = now->instanceAutomation[actual];
            }
            return false;
        });
        for (const auto& control : live->controls)
            if (!target.control(control.port) && !previous->control(control.port) && control.revision > cutoff) {
                auto added = control;
                if (replacingState) added.fromState = false;
                target.controls.push_back(std::move(added));
            }
        for (const auto& control : target.controls) target.settingsRevision = std::max(target.settingsRevision, control.revision);
    };
    std::vector<EffectOwner> owners;
    forEachEffectRack(result, [&](EffectOwner owner, const auto&) { owners.push_back(owner); });
    for (const auto owner : owners) {
        auto edit = prepareEffectRackEdit(result, owner);
        if (!edit) throw std::runtime_error("cannot merge history for an invalid effect owner");
        for (auto& insert : edit->inserts) merge(insert);
        std::string error;
        if (!replaceEffectRack(result, *edit, error)) throw std::runtime_error(error);
    }
    return result;
}

void restoreState(Project& project, const SessionState& state)
{
    if (state.sampleRate) project.setSampleRate(state.sampleRate);
    project.setDefaultExportBits(state.defaultExportBits);
    project.metadata() = state.metadata;
    project.video() = state.video;
    project.musicalTime() = state.musicalTime;
    project.restoreClipGroups(state.clipGroups);
    // Rebuilt rather than diffed. A diff would have to get insertion order, removal
    // and reordering all right to end up somewhere a rebuild reaches by construction,
    // and this runs once per undo -- never on the audio thread, never in a loop.
    // Heights survive by id: they are not part of the snapshot (see the header).
    std::vector<std::pair<TrackId, TrackHeight>> heights;
    heights.reserve(project.tracks().size());
    for (const Track& t : project.tracks())
        heights.emplace_back(t.id, t.height);

    project.tracks().clear();

    for (std::size_t i = 0; i < state.tracks.size(); ++i) {
        const TrackState& ts = state.tracks[i];
        Track t;
        t.id = ts.id;
        t.name = ts.name;
        t.role = ts.role;
        t.colour = ts.colour;
        for (const auto& [id, height] : heights) {
            if (id == ts.id)
                t.height = height;
        }
        t.gain = ts.gain;
        t.pan = ts.pan;
        t.muted = ts.muted;
        t.soloed = ts.soloed;
        t.volumeAutomation = ts.volumeAutomation;
        t.panAutomation = ts.panAutomation;
        t.clips = ts.clips;
        t.inserts = ts.inserts;
        t.routing = ts.routing;
        t.ducking = ts.ducking;
        project.insertTrack(i, std::move(t));
    }

    project.markers().clear();
    for (const Marker& m : state.markers)
        project.insertMarker(m);

    project.masterInserts() = state.masterInserts;
    project.masterVolumeAutomation() = state.masterVolumeAutomation;
}

} // namespace omawave::session
