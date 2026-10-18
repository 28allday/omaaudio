#pragma once

#include "effect_owner.h"
#include "project.h"
#include "state.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omawave::session {
namespace effect_rack_detail {
inline const auto& tracks(const Project& value) noexcept { return value.tracks(); }
inline auto& tracks(Project& value) noexcept { return value.tracks(); }
inline const auto& tracks(const SessionState& value) noexcept { return value.tracks; }
inline auto& tracks(SessionState& value) noexcept { return value.tracks; }
inline const auto& master(const Project& value) noexcept { return value.masterInserts(); }
inline auto& master(Project& value) noexcept { return value.masterInserts(); }
inline const auto& master(const SessionState& value) noexcept { return value.masterInserts; }
inline auto& master(SessionState& value) noexcept { return value.masterInserts; }

// No allocation, including callers such as automationPointCount() noexcept.
// Active regions are usually a small fraction of clips; the first-view scan
// also deduplicates malformed nonadjacent aliases until validation rejects them.
template<class State>
bool firstView(const State& state, const Clip& current) noexcept
{
    for (const auto& track : tracks(state)) for (const auto& clip : track.clips) {
        if (&clip == &current) return true;
        if (clip.rack && clip.rack->owner == current.rack->owner) return false;
    }
    return false;
}

template<class State, class Visitor>
void observe(const State& state, Visitor&& visit)
{
    visit(EffectOwner::master(), master(state));
    for (const auto& track : tracks(state)) {
        visit(EffectOwner::track(track.id), track.inserts);
        for (const auto& clip : track.clips)
            if (clip.rack && firstView(state, clip)) visit(clip.rack->owner, clip.rack->inserts);
    }
}

template<class State>
const std::vector<Insert>* find(const State& state, EffectOwner owner) noexcept
{
    if (owner.isMaster()) return &master(state);
    if (owner.isTrack()) {
        for (const auto& track : tracks(state)) if (track.id == owner.trackId()) return &track.inserts;
    } else if (owner.isClip()) {
        const ClipRack* rack = nullptr;
        for (const auto& track : tracks(state)) for (const auto& clip : track.clips) {
            if (!clip.rack || clip.rack->owner != owner) continue;
            // Conflicting immutable versions of one live owner are not a rack.
            if (rack && rack != clip.rack.get()) return nullptr;
            rack = clip.rack.get();
        }
        if (rack) return &rack->inserts;
    }
    return nullptr;
}

template<class State>
std::vector<Insert>* findLegacy(State& state, EffectOwner owner) noexcept
{
    if (owner.isMaster()) return &master(state);
    if (owner.isTrack())
        for (auto& track : tracks(state)) if (track.id == owner.trackId()) return &track.inserts;
    return nullptr;
}

template<class State>
std::string validate(const State& state)
{
    struct Region { TrackId track; const Clip* last; };
    std::unordered_map<EffectOwner, Region> regions;
    std::unordered_map<std::uint64_t, const Clip*> views;
    for (const auto& track : tracks(state)) for (const auto& clip : track.clips) {
        if (!views.emplace(clip.identity.value(), &clip).second)
            return "a clip view identity occurs more than once";
        if (!clip.hasValidEffects()) return "invalid clip effect origin or view coordinates";
        if (!clip.rack) continue;
        if (clip.timelineStart < 0 || clip.length > std::numeric_limits<FramePos>::max() - clip.timelineStart)
            return "clip effect view exceeds the timeline";
        const auto [entry, first] = regions.emplace(clip.rack->owner, Region{track.id, &clip});
        if (!first) {
            const auto& previous = *entry->second.last;
            if (entry->second.track != track.id || previous.rack != clip.rack
                || previous.timelineEnd() != clip.timelineStart
                || previous.effectOffset + previous.length != clip.effectOffset)
                return "one clip effect owner has incompatible live views";
            entry->second.last = &clip;
        } else if (const auto error = validateAutomation(clip.rack->inserts); !error.empty()) return error;
    }
    return {};
}

inline std::size_t insertPoints(const std::vector<Insert>& inserts) noexcept
{
    std::size_t count = 0;
    for (const auto& insert : inserts) for (const auto& control : insert.controls)
        count += control.automationPointCount();
    return count;
}
inline std::size_t points(const Project& project) noexcept { return project.automationPointCount(); }
inline std::size_t points(const SessionState& state) noexcept
{
    std::size_t count = state.masterVolumeAutomation.points.size();
    observe(state, [&](EffectOwner, const auto& inserts) { count += insertPoints(inserts); });
    for (const auto& track : state.tracks) {
        count += track.volumeAutomation.points.size() + track.panAutomation.points.size();
        for (const auto& clip : track.clips)
            count += clip.volumeEnvelope.points.size() + clip.panEnvelope.points.size();
    }
    return count;
}
inline bool retained(const SessionState&, const ClipRack*) noexcept { return false; }
inline bool retained(const Project& project, const ClipRack* rack) noexcept
{
    if (!rack) return false;
    for (const auto& source : project.sources()) if (source && source->crossfade)
        if (source->crossfade->left.rack.get() == rack || source->crossfade->right.rack.get() == rack) return true;
    return false;
}
} // namespace effect_rack_detail

// All-owner observation is const, even when the caller owns a mutable Project.
// Retained source recipes are immutable rendering inputs, not live owners.
template<class Visitor>
void forEachEffectRack(const Project& project, Visitor&& visit)
{ effect_rack_detail::observe(project, std::forward<Visitor>(visit)); }
template<class Visitor>
void forEachEffectRack(const SessionState& state, Visitor&& visit)
{ effect_rack_detail::observe(state, std::forward<Visitor>(visit)); }

inline const std::vector<Insert>* findEffectRack(const Project& project, EffectOwner owner) noexcept
{ return effect_rack_detail::find(project, owner); }
inline const std::vector<Insert>* findEffectRack(const SessionState& state, EffectOwner owner) noexcept
{ return effect_rack_detail::find(state, owner); }

// Stored immutable versions include recipe inputs. Deduplicate by immutable
// object, not owner: a recipe can retain an older version of a live region.
template<class Visitor>
void forEachStoredClipRack(const Project& project, Visitor&& visit)
{
    const auto clips = [&](auto&& each) {
        for (const auto& track : project.tracks()) for (const auto& clip : track.clips) each(clip);
        for (const auto& source : project.sources()) if (source && source->crossfade) {
            each(source->crossfade->left); each(source->crossfade->right);
        }
    };
    clips([&](const Clip& current) {
        if (!current.rack) return;
        bool before = true, seen = false;
        clips([&](const Clip& previous) {
            if (&previous == &current) before = false;
            if (before && previous.rack == current.rack) seen = true;
        });
        if (!seen) visit(*current.rack);
    });
}

inline std::string validateClipRacks(const Project& project)
{
    if (const auto error = effect_rack_detail::validate(project); !error.empty()) return error;
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips) if (clip.rack) {
        const auto* source = project.source(clip.source);
        if (!source || !clip.validSource(static_cast<FramePos>(source->audio.frames())))
            return "a clip effect view is outside its source audio";
    }
    return {};
}
inline std::string validateClipRacks(const SessionState& state)
{ return effect_rack_detail::validate(state); }

// Explicit compatibility boundary: clip racks can never be obtained mutably.
inline std::vector<Insert>* findLegacyEffectRack(Project& project, EffectOwner owner) noexcept
{ return effect_rack_detail::findLegacy(project, owner); }
inline std::vector<Insert>* findLegacyEffectRack(SessionState& state, EffectOwner owner) noexcept
{ return effect_rack_detail::findLegacy(state, owner); }
template<class Visitor>
void forEachLegacyEffectRack(Project& project, Visitor&& visit)
{
    visit(EffectOwner::master(), project.masterInserts());
    for (auto& track : project.tracks()) visit(EffectOwner::track(track.id), track.inserts);
}
template<class Visitor>
void forEachLegacyEffectRack(SessionState& state, Visitor&& visit)
{
    visit(EffectOwner::master(), state.masterInserts);
    for (auto& track : state.tracks) visit(EffectOwner::track(track.id), track.inserts);
}

// Only inserts is the proposed output. The remaining fields pin the complete
// accepted baseline, including provenance excluded from persistent equality.
struct EffectRackEdit {
    EffectOwner owner{};
    std::vector<Insert> expected{}, inserts{};
    std::shared_ptr<const ClipRack> expectedRack{};
    std::vector<std::pair<TrackId, Clip>> expectedViews{};
};

namespace effect_rack_detail {
template<class State>
std::optional<EffectRackEdit> prepare(const State& state, EffectOwner owner)
{
    const auto* inserts = findEffectRack(state, owner);
    if (!inserts) return {};
    if (owner.isClip() && !validateClipRacks(state).empty()) return {};
    EffectRackEdit result{owner, *inserts, *inserts, {}, {}};
    if (owner.isClip()) for (const auto& track : tracks(state)) for (const auto& clip : track.clips) {
        if (!clip.rack || clip.rack->owner != owner) continue;
        if (!clip.hasValidEffects()) return {};
        result.expectedRack = clip.rack;
        result.expectedViews.emplace_back(track.id, clip);
    }
    return result;
}

template<class State>
bool replace(State& state, const EffectRackEdit& edit, std::string& error)
{
    error.clear();
    const auto* current = findEffectRack(state, edit.owner);
    if (!current || !sameInsertDescriptors(*current, edit.expected)) {
        error = "the effect owner or its settings changed"; return false;
    }
    if (const auto reason = validateAutomation(edit.inserts); !reason.empty()) {
        error = reason; return false;
    }
    const auto count = points(state);
    const auto previousPoints = insertPoints(edit.expected);
    const auto proposedPoints = insertPoints(edit.inserts);
    const bool unchanged = sameInsertDescriptors(*current, edit.inserts);
    const auto removed = !unchanged && edit.owner.isClip() && retained(state, edit.expectedRack.get()) ? 0 : previousPoints;
    if (count < removed || count - removed > kMaxProjectAutomationPoints
        || proposedPoints > kMaxProjectAutomationPoints - (count - removed)) {
        error = "effect automation exceeds the project point limit"; return false;
    }
    for (std::size_t i = 0; i < edit.inserts.size(); ++i) {
        if (edit.inserts[i].uri.empty() || edit.inserts[i].instanceState.size() > 2) {
            error = "invalid effect insert or instance state count"; return false;
        }
        for (std::size_t j = 0; j < i; ++j) if (edit.inserts[i].identity == edit.inserts[j].identity) {
            error = "an effect insert identity occurs twice in one rack"; return false;
        }
        for (const auto& opaque : edit.inserts[i].instanceState)
            if (!opaque || opaque.uri() != edit.inserts[i].uri) {
                error = "effect state belongs to another plug-in"; return false;
            }
        for (std::size_t port = 0; port < edit.inserts[i].controls.size(); ++port) {
            const auto& control = edit.inserts[i].controls[port];
            if (control.fromState && edit.inserts[i].instanceState.empty()) {
                error = "state-authoritative control has no plug-in state"; return false;
            }
            for (std::size_t previous = 0; previous < port; ++previous)
                if (edit.inserts[i].controls[previous].port == control.port) {
                    error = "an effect control occurs more than once"; return false;
                }
        }
    }
    if (!edit.owner.isClip()) {
        auto* destination = findLegacyEffectRack(state, edit.owner);
        if (!destination || edit.expectedRack || !edit.expectedViews.empty()) {
            error = "invalid effect rack transaction"; return false;
        }
        if (sameInsertDescriptors(*destination, edit.inserts)) return true;
        auto replacement = edit.inserts; // allocate before mutating the model
        destination->swap(replacement);
        return true;
    }
    if (!edit.expectedRack || edit.expectedRack->owner != edit.owner || edit.expectedViews.empty()) {
        error = "invalid clip effect transaction"; return false;
    }
    std::size_t index = 0;
    for (const auto& track : tracks(state)) for (const auto& clip : track.clips) {
        if (!clip.rack || clip.rack->owner != edit.owner) continue;
        if (index >= edit.expectedViews.size() || edit.expectedViews[index].first != track.id
            || clip.rack != edit.expectedRack || !clip.sameDescriptor(edit.expectedViews[index].second)) {
            error = "the clip effect region changed"; return false;
        }
        ++index;
    }
    if (index != edit.expectedViews.size()) { error = "the clip effect region changed"; return false; }
    if (const auto reason = validateClipRacks(state); !reason.empty()) { error = reason; return false; }
    if (sameInsertDescriptors(*current, edit.inserts)) return true;
    auto candidate = std::make_shared<ClipRack>(*edit.expectedRack);
    candidate->inserts = edit.inserts; // every allocation precedes alias replacement
    std::shared_ptr<const ClipRack> replacement = std::move(candidate);
    for (auto& track : tracks(state)) for (auto& clip : track.clips)
        if (clip.rack && clip.rack->owner == edit.owner) clip.rack = replacement;
    return true;
}
} // namespace effect_rack_detail

inline std::optional<EffectRackEdit> prepareEffectRackEdit(const Project& project, EffectOwner owner)
{ return effect_rack_detail::prepare(project, owner); }
inline std::optional<EffectRackEdit> prepareEffectRackEdit(const SessionState& state, EffectOwner owner)
{ return effect_rack_detail::prepare(state, owner); }
inline bool replaceEffectRack(Project& project, const EffectRackEdit& edit, std::string& error)
{ return effect_rack_detail::replace(project, edit, error); }
inline bool replaceEffectRack(SessionState& state, const EffectRackEdit& edit, std::string& error)
{ return effect_rack_detail::replace(state, edit, error); }

} // namespace omawave::session
