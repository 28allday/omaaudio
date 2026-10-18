#include "project.h"
#include "effect_racks.h"
#include "routing.h"

#include <algorithm>
#include <utility>
#include <stdexcept>
#include <set>
#include <limits>

namespace omawave::session {

bool ClipGroup::valid() const noexcept
{
    if (std::uint32_t(id) > std::uint32_t(std::numeric_limits<std::int32_t>::max())
        || name.empty() || name.size() > 512) return false;
    // Names are TOML text. Reject malformed UTF-8 before saving can create a
    // project whose metadata cannot be read back (spaces are allowed here).
    for (std::size_t at = 0; at < name.size();) {
        const auto first = static_cast<unsigned char>(name[at++]);
        if (first < 32 || first == 127) return false;
        if (first < 128) continue;
        unsigned continuation = 0; std::uint32_t value = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { continuation = 1; value = first & 0x1F; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { continuation = 2; value = first & 0x0F; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { continuation = 3; value = first & 0x07; minimum = 0x10000; }
        else return false;
        if (continuation > name.size() - at) return false;
        for (unsigned n = 0; n < continuation; ++n) {
            const auto next = static_cast<unsigned char>(name[at++]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
    }
    return colour.empty() || (colour.size() == 7 && colour[0] == '#'
        && std::all_of(colour.begin() + 1, colour.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }));
}

ClipGroup* Project::clipGroup(ClipGroupId id) noexcept
{
    for (auto& group : clipGroups_) if (group.id == id) return &group;
    return nullptr;
}
const ClipGroup* Project::clipGroup(ClipGroupId id) const noexcept
{
    for (const auto& group : clipGroups_) if (group.id == id) return &group;
    return nullptr;
}
ClipGroupId Project::addClipGroup(std::string name, std::string colour)
{
    ClipGroup group{ClipGroupId(nextClipGroupId_), std::move(name), std::move(colour)};
    if (!insertClipGroup(group)) return kInvalidClipGroup;
    return group.id;
}
bool Project::insertClipGroup(ClipGroup group)
{
    if (!group.valid() || clipGroup(group.id)) return false;
    const auto next = std::uint32_t(group.id) + 1;
    clipGroups_.push_back(std::move(group)); nextClipGroupId_ = std::max(nextClipGroupId_, next);
    return true;
}
bool Project::removeClipGroup(ClipGroupId id)
{
    if (!clipGroup(id)) return false;
    for (auto& track : tracks_) for (auto& clip : track.clips)
        if (clip.group == id) clip.group = kInvalidClipGroup;
    std::erase_if(clipGroups_, [id](const auto& group) { return group.id == id; });
    return true;
}
void Project::restoreClipGroups(const std::vector<ClipGroup>& groups)
{
    clipGroups_ = groups;
    for (const auto& group : groups)
        nextClipGroupId_ = std::max(nextClipGroupId_, std::uint32_t(group.id) < 0x80000000u
            ? std::uint32_t(group.id) + 1 : 0x80000000u);
}
std::string Project::validateClipGroups() const
{
    std::set<ClipGroupId> ids;
    for (const auto& group : clipGroups_)
        if (!group.valid() || !ids.insert(group.id).second) return "invalid or duplicate clip group";
    for (const auto& track : tracks_) for (const auto& clip : track.clips)
        if (clip.group != kInvalidClipGroup && !ids.contains(clip.group)) return "a clip refers to a missing group";
    return {};
}

SourceId Project::addSource(std::string name, std::string originalPath, AudioBuffer audio,
                            std::optional<std::uint32_t> originalSampleRate, Uuid uuid,
                            std::optional<CrossfadeRecipe> crossfade, std::uint32_t storedRate,
                            AudioMetadata provenance)
{
    const auto id = static_cast<SourceId>(sources_.size());
    // Loading passes the uuid the project was saved with; importing passes none and
    // gets a fresh one.
    if (uuid.isNull())
        uuid = Uuid::generate();

    sources_.push_back(std::make_shared<const AudioSource>(AudioSource{
        .id = id,
        .uuid = uuid,
        .name = std::move(name),
        .originalPath = std::move(originalPath),
        .audio = std::move(audio),
        .sampleRate = storedRate ? storedRate : sampleRate_,
        .originalSampleRate = originalSampleRate,
        .crossfade = std::move(crossfade),
        .provenance = std::move(provenance),
    }));
    return id;
}

const AudioSource* Project::source(SourceId id) const noexcept
{
    const auto index = static_cast<std::uint32_t>(id);
    if (index >= sources_.size())
        return nullptr;
    return sources_[index].get();
}

const AudioSource* Project::sourceByUuid(const Uuid& uuid) const noexcept
{
    for (const auto& s : sources_) {
        if (s && s->uuid == uuid)
            return s.get();
    }
    return nullptr;
}

std::optional<SourceId> Project::sourceIdByUuid(const Uuid& uuid) const noexcept
{
    const AudioSource* found = sourceByUuid(uuid);
    return found == nullptr ? std::nullopt : std::optional<SourceId>(found->id);
}

std::vector<bool> Project::usedSources(const std::vector<SourceId>& protectedSources) const
{
    std::vector<bool> used(sources_.size(), false);
    auto pending = protectedSources;
    for (const auto& track : tracks_) for (const auto& clip : track.clips) pending.push_back(clip.source);
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        const auto* audio = source(id);
        if (!audio) throw std::runtime_error("cleanup found a missing protected audio source");
        if (used[std::size_t(id)]) continue;
        used[std::size_t(id)] = true;
        if (audio->crossfade) {
            pending.push_back(audio->crossfade->left.source);
            pending.push_back(audio->crossfade->right.source);
        }
    }
    return used;
}

std::size_t Project::pruneSources(const std::vector<SourceId>& protectedSources)
{
    const auto used = usedSources(protectedSources);
    std::size_t removed = 0;
    for (std::size_t i = 0; i < sources_.size(); ++i) if (sources_[i] && !used[i]) {
        sources_[i].reset(); ++removed;
    }
    return removed;
}

TrackId Project::addTrack(std::string name)
{
    const auto id = static_cast<TrackId>(nextTrackId_++);
    // Assigned rather than brace-initialised: a designated initialiser that skips
    // defaulted members trips -Wmissing-field-initializers, and section 8 keeps
    // -Wextra -Werror on.
    Track t;
    t.id = id;
    t.name = std::move(name);
    tracks_.push_back(std::move(t));
    return id;
}

void Project::insertTrack(std::size_t index, Track track)
{
    // Undo re-adds a track with the id it had. The counter only moves forward, so a
    // later addTrack can never collide with it.
    const auto raw = static_cast<std::uint32_t>(track.id);
    if (track.id != kInvalidTrack && raw >= nextTrackId_)
        nextTrackId_ = raw + 1;

    const std::size_t at = std::min(index, tracks_.size());
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(at), std::move(track));
}

bool Project::removeTrack(TrackId id)
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [id](const Track& t) { return t.id == id; });
    if (it == tracks_.end())
        return false;
    tracks_.erase(it);
    removeRoutingTarget(*this, id);
    return true;
}

bool Project::moveTrack(TrackId id, std::size_t toIndex)
{
    const std::optional<std::size_t> from = trackIndex(id);
    if (!from.has_value() || tracks_.empty())
        return false;

    const std::size_t to = std::min(toIndex, tracks_.size() - 1);
    if (to == *from)
        return true;

    Track moved = std::move(tracks_[*from]);
    tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(*from));
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return true;
}

Track* Project::track(TrackId id) noexcept
{
    for (Track& t : tracks_) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

const Track* Project::track(TrackId id) const noexcept
{
    for (const Track& t : tracks_) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

std::optional<std::size_t> Project::trackIndex(TrackId id) const noexcept
{
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == id)
            return i;
    }
    return std::nullopt;
}

bool Project::anySoloed() const noexcept
{
    return std::any_of(tracks_.begin(), tracks_.end(),
                       [](const Track& t) { return t.soloed; });
}

MarkerId Project::addMarker(Marker marker)
{
    marker.id = reserveMarkerId();
    const MarkerId id = marker.id;
    markers_.push_back(std::move(marker));
    sortMarkers();
    return id;
}

MarkerId Project::reserveMarkerId() noexcept
{
    return static_cast<MarkerId>(nextMarkerId_++);
}

void Project::insertMarker(Marker marker)
{
    const auto raw = static_cast<std::uint32_t>(marker.id);
    if (marker.id != kInvalidMarker && raw >= nextMarkerId_)
        nextMarkerId_ = raw + 1;
    markers_.push_back(std::move(marker));
    sortMarkers();
}

bool Project::removeMarker(MarkerId id)
{
    const auto it = std::find_if(markers_.begin(), markers_.end(),
                                 [id](const Marker& m) { return m.id == id; });
    if (it == markers_.end())
        return false;
    markers_.erase(it);
    return true;
}

Marker* Project::marker(MarkerId id) noexcept
{
    for (Marker& m : markers_) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

const Marker* Project::marker(MarkerId id) const noexcept
{
    for (const Marker& m : markers_) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

void Project::sortMarkers()
{
    // Stable, so two markers on the same frame keep the order they were made in
    // rather than swapping every time one of them is touched.
    std::stable_sort(markers_.begin(), markers_.end(),
                     [](const Marker& a, const Marker& b) {
                         return a.position < b.position;
                     });
}

FramePos Project::length() const noexcept
{
    FramePos longest = 0;
    for (const Track& t : tracks_) {
        const FramePos len = t.length();
        if (len > longest)
            longest = len;
    }
    return longest;
}

std::size_t Project::automationPointCount() const noexcept
{
    std::size_t total=masterVolumeAutomation_.points.size();
    const auto insertPoints = [&](const auto& inserts) {
        for (const auto& insert:inserts) for (const auto& control:insert.controls) total+=control.automationPointCount();
    };
    insertPoints(masterInserts_);
    forEachStoredClipRack(*this, [&](const ClipRack& rack) { insertPoints(rack.inserts); });
    const auto clipPoints = [](const Clip& clip) { return clip.volumeEnvelope.points.size()+clip.panEnvelope.points.size(); };
    for (const auto& track:tracks_) {
        insertPoints(track.inserts);
        total+=track.volumeAutomation.points.size()+track.panAutomation.points.size();
        for (const auto& clip:track.clips) total+=clipPoints(clip);
    }
    for (const auto& source:sources_) if (source && source->crossfade)
        total+=clipPoints(source->crossfade->left)+clipPoints(source->crossfade->right);
    return total;
}

} // namespace omawave::session
