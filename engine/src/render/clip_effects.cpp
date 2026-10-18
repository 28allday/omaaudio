#include "clip_effects.h"

#include "engine/clip_effect_runtime.h"
#include "session/clip_reader.h"
#include "session/disk_audio.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace omawave::render {
using session::FramePos;

bool clipEffectsActive(const session::ClipRack& rack) noexcept
{
    return std::any_of(rack.inserts.begin(), rack.inserts.end(),
                       [](const session::Insert& insert) { return !insert.bypass; });
}

namespace {
ClipEffectRegion regionOf(const session::Project& project, const session::Clip& clip)
{
    if (!clip.rack || !clip.hasValidEffects())
        throw std::runtime_error("a clip's effect region does not match its view");
    const auto* source = project.source(clip.source);
    const std::uint32_t channels = source ? std::max<std::uint32_t>(1, source->audio.channels()) : 1;
    if (clip.timelineStart < std::numeric_limits<FramePos>::min() + clip.effectOffset)
        throw std::runtime_error("clip effect origin overflows the timeline");
    return {clip.rack, clip.timelineStart - clip.effectOffset, channels,
            project.sampleRate(), project.musicalTime()};
}

const char* statusText(engine::ClipEffectRuntime::Status status)
{
    using Status = engine::ClipEffectRuntime::Status;
    switch (status) {
    case Status::Ready: return "ready";
    case Status::NeedsPreparation: return "an effect stopped processing";
    case Status::InvalidRange: return "invalid effect range";
    case Status::InvalidContext: return "invalid transport context";
    case Status::ProgramChanged: return "an effect changed its configuration while rendering";
    case Status::LatencyChanged: return "an effect changed its delay while rendering";
    }
    return "unknown effect failure";
}
} // namespace

std::vector<ClipEffectRegion> clipEffectRegions(const session::Project& project)
{
    std::vector<ClipEffectRegion> result;
    for (const auto& track : project.tracks())
        for (const auto& clip : track.clips) {
            if (!clip.rack || !clipEffectsActive(*clip.rack)) continue;
            auto region = regionOf(project, clip);
            if (std::find(result.begin(), result.end(), region) == result.end())
                result.push_back(std::move(region));
        }
    return result;
}

std::unique_ptr<lv2::Chain> buildClipEffectChain(const lv2::World& world,
    lv2::HostFeatures& features, const ClipEffectRegion& region, std::string& error)
{
    error.clear();
    if (!region.rack || !region.channels || region.channels > engine::ClipEffectRuntime::maxChannels) {
        error = "unsupported clip effect channel layout";
        return nullptr;
    }
    // A mono clip through an effect that only works in stereo becomes stereo,
    // as it would through a stereo track insert. Mono-capable effects keep it mono.
    std::uint32_t width = region.channels;
    if (width == 1)
        for (const auto& insert : region.rack->inserts) {
            if (insert.bypass) continue;
            const auto* info = world.find(insert.uri);
            if (info && !info->fits(1)) { width = 2; break; }
        }
    auto chain = std::make_unique<lv2::Chain>(width, lv2::kMaxBlock, host::ProcessingMode::Offline);
    for (std::size_t slot = 0; slot < region.rack->inserts.size(); ++slot) {
        const auto& insert = region.rack->inserts[slot];
        if (insert.bypass) { chain->appendMissing(insert.identity); continue; }
        std::string reason;
        if (!chain->append(world, features, insert, reason)) {
            error = "Clip effect " + std::to_string(slot + 1) + " (" + insert.uri + "): " + reason;
            return nullptr;
        }
    }
    return chain;
}

ClipEffectRender renderClipEffectRegion(const session::Project& project,
    const ClipEffectRegion& region, lv2::Chain& chain,
    const std::atomic<bool>* cancel, const std::function<void(float)>& progress)
{
    ClipEffectRender result;
    const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    const auto fail = [&](std::string error) { result.error = std::move(error); result.audio = {}; return result; };
    if (!region.rack || !region.rack->validOrigin()) return fail("invalid clip effect region");
    if (chain.channels() < region.channels) return fail("clip effect chain has the wrong channel count");
    if (!chain.warmUp(cancel)) {
        result.cancelled = stopped();
        return fail(result.cancelled ? "render cancelled" : "clip effect warm-up failed");
    }
    std::unique_ptr<engine::ClipEffectRuntime> runtime;
    try {
        runtime = std::make_unique<engine::ClipEffectRuntime>(region.rack, chain, region.origin);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    const auto& rack = *region.rack;
    const auto latency = FramePos(runtime->latencyFrames());
    const auto length = rack.length;
    const auto channels = chain.channels(); // processing width; a mono source feeds every input

    // The dry input that actually exists. Context outside the source is silence.
    const auto* source = project.source(rack.source);
    FramePos sourceFrames = 0;
    if (source) sourceFrames = rack.elastic ? rack.elastic->outputFrames : FramePos(source->audio.frames());
    if (source && source->audio.channels() != region.channels) return fail("clip effect source channel count changed");
    const FramePos validBegin = std::clamp<FramePos>(-rack.sourceOffset, 0, length);
    const FramePos validEnd = std::clamp<FramePos>(sourceFrames - rack.sourceOffset, validBegin, length);
    session::Clip dry;
    dry.source = rack.source;
    dry.elastic = rack.elastic;
    dry.sourceOffset = rack.sourceOffset + validBegin;
    dry.length = validEnd - validBegin;
    dry.timelineStart = 0;
    session::ClipReader reader(project);

    std::shared_ptr<session::DiskAudio> output;
    try {
        output = session::DiskAudio::create(channels, std::size_t(length));
    } catch (const std::exception& error) {
        return fail(std::string("could not store clip effect audio: ") + error.what());
    }
    std::array<std::array<float, lv2::kMaxBlock>, engine::ClipEffectRuntime::maxChannels> input{};
    std::array<const float*, engine::ClipEffectRuntime::maxChannels> planes{};
    for (std::uint32_t c = 0; c < channels; ++c) planes[c] = input[c].data();
    const auto total = length + latency;
    const auto blockLimit = std::min<std::uint32_t>({runtime->maxBlock(), lv2::kMaxBlock, session::ClipReader::kBlock});
    try {
        for (FramePos local = 0; local < total;) {
            if (stopped()) { result.cancelled = true; return fail("render cancelled"); }
            const auto count = std::uint32_t(std::min<FramePos>(blockLimit, total - local));
            for (std::uint32_t c = 0; c < channels; ++c) std::fill_n(input[c].data(), count, 0.0f);
            const auto from = std::max(local, validBegin), to = std::min(local + FramePos(count), validEnd);
            if (from < to && dry.length > 0) {
                reader.read(dry, from - validBegin, std::uint32_t(to - from), cancel);
                for (std::uint32_t c = 0; c < channels; ++c)
                    std::copy_n(reader.plane(std::min(c, reader.channels() - 1)).data(), std::size_t(to - from),
                                input[c].data() + (from - local));
            }
            host::ProcessContext context;
            context.time = {region.origin + local, 0, 0, true, true};
            if (region.musicalTime.enabled && region.musicalTime.valid()) {
                context.tempo = region.musicalTime.tempoMilli / 1000.;
                context.numerator = std::uint16_t(region.musicalTime.beats);
                context.denominator = std::uint16_t(region.musicalTime.unit);
            }
            const auto status = runtime->process(local, std::span(planes).first(channels), count, context);
            if (status != engine::ClipEffectRuntime::Status::Ready) return fail(statusText(status));
            // Output local n is the processed input n - latency.
            const auto keepFrom = std::max(local, latency), keepTo = std::min(local + FramePos(count), total);
            if (keepFrom < keepTo)
                for (std::uint32_t c = 0; c < channels; ++c)
                    output->writePlane(c, std::size_t(keepFrom - latency),
                        runtime->output(c).subspan(std::size_t(keepFrom - local), std::size_t(keepTo - keepFrom)));
            local += count;
            if (progress) progress(float(local) / float(total));
        }
        output->seal();
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    result.audio = session::AudioBuffer::fromDisk(std::move(output));
    result.ok = true;
    return result;
}

void ClipEffectRenders::insert(const ClipEffectRegion& region, session::AudioBuffer audio)
{
    if (!region.rack || audio.channels() < region.channels || audio.frames() != std::size_t(region.rack->length))
        throw std::invalid_argument("clip effect rendering does not match its region");
    auto shared = std::make_shared<const session::AudioBuffer>(std::move(audio));
    std::erase_if(entries_, [&](const Entry& entry) { return entry.region == region; });
    entries_.push_back({region, std::move(shared)});
}

const session::AudioBuffer* ClipEffectRenders::find(const ClipEffectRegion& region) const noexcept
{
    for (const auto& entry : entries_)
        if (entry.region == region) return entry.audio.get();
    return nullptr;
}

const session::AudioBuffer* ClipEffectRenders::findStandIn(const ClipEffectRegion& region) const noexcept
{
    if (!region.rack) return nullptr;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        const auto& other = it->region;
        if (other.rack && other.rack->owner == region.rack->owner && other.origin == region.origin
            && other.channels == region.channels && other.sampleRate == region.sampleRate
            && other.rack->source == region.rack->source && other.rack->sourceOffset == region.rack->sourceOffset
            && other.rack->length == region.rack->length && other.rack->elastic == region.rack->elastic)
            return it->audio.get();
    }
    return nullptr;
}

std::size_t ClipEffectRenders::retain(const std::vector<ClipEffectRegion>& wanted)
{
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const Entry& entry) {
        return std::find(wanted.begin(), wanted.end(), entry.region) == wanted.end();
    });
    return before - entries_.size();
}

std::size_t ClipEffectRenders::trim(const std::vector<ClipEffectRegion>& wanted, std::size_t recent)
{
    const auto before = entries_.size();
    std::size_t kept = 0;
    for (auto it = entries_.end(); it != entries_.begin();) {
        --it;
        if (std::find(wanted.begin(), wanted.end(), it->region) != wanted.end()) continue;
        if (kept < recent) { ++kept; continue; }
        it = entries_.erase(it);
    }
    return before - entries_.size();
}

std::size_t ClipEffectRenders::bytes() const noexcept
{
    std::size_t total = 0;
    for (const auto& entry : entries_)
        total += entry.audio->diskStorage() ? entry.audio->diskStorage()->bytes() : entry.audio->memoryBytes();
    return total;
}

AppliedClipEffects applyClipEffects(const session::Project& project,
    const ClipEffectRenders& renders, MissingClipEffects policy)
{
    AppliedClipEffects result;
    result.project = project;
    std::vector<std::pair<const session::AudioBuffer*, session::SourceId>> added;
    for (auto& track : result.project.tracks()) {
        std::vector<session::Clip> kept;
        kept.reserve(track.clips.size());
        for (auto clip : track.clips) {
            if (!clip.rack) { kept.push_back(std::move(clip)); continue; }
            if (!clipEffectsActive(*clip.rack)) {
                // Settings are retained in the model; the sound is the dry view.
                clip.rack.reset(); clip.effectOffset = 0;
                kept.push_back(std::move(clip));
                continue;
            }
            const auto region = regionOf(project, clip);
            const auto* audio = renders.find(region);
            if (!audio) {
                if (policy == MissingClipEffects::Refuse) throw std::runtime_error("clip effects have not been rendered");
                if (std::find(result.missing.begin(), result.missing.end(), region) == result.missing.end())
                    result.missing.push_back(region);
                if (policy == MissingClipEffects::StandIn) audio = renders.findStandIn(region);
                if (!audio) continue;
            }
            auto known = std::find_if(added.begin(), added.end(), [&](const auto& item) { return item.first == audio; });
            session::SourceId id;
            if (known != added.end()) id = known->second;
            else {
                const auto* dry = project.source(clip.source);
                id = result.project.addSource((dry ? dry->name : std::string("Clip")) + " (clip effects)",
                                              {}, *audio, std::nullopt, session::Uuid{}, std::nullopt,
                                              project.sampleRate());
                added.emplace_back(audio, id);
            }
            clip.source = id;
            clip.sourceOffset = clip.effectOffset;
            clip.elastic.reset();
            clip.rack.reset();
            clip.effectOffset = 0;
            kept.push_back(std::move(clip));
        }
        track.clips = std::move(kept);
    }
    return result;
}

PreparedClipEffects prepareClipEffects(const session::Project& project,
    ClipEffectRenders& renders, const std::vector<ClipEffectRegion>& regions,
    const std::vector<lv2::Chain*>& chains, const std::atomic<bool>* cancel,
    const std::function<void(float)>& progress)
{
    PreparedClipEffects result;
    if (chains.size() != regions.size()) { result.error = "clip effect chains do not match their regions"; return result; }
    std::size_t needed = 0;
    for (const auto& region : regions) needed += renders.find(region) ? 0 : 1;
    std::size_t done = 0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (renders.find(regions[i])) continue;
        if (!chains[i]) { result.error = "clip effects could not be prepared"; return result; }
        const auto base = float(done) / float(std::max<std::size_t>(1, needed));
        auto rendered = renderClipEffectRegion(project, regions[i], *chains[i], cancel,
            [&](float value) { if (progress) progress(base + value / float(std::max<std::size_t>(1, needed))); });
        if (!rendered.ok) {
            result.cancelled = rendered.cancelled;
            result.error = rendered.error;
            return result;
        }
        renders.insert(regions[i], std::move(rendered.audio));
        ++done;
    }
    try {
        result.project = applyClipEffects(project, renders).project;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace omawave::render
