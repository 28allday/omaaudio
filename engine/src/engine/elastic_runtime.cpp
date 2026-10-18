#include "elastic_runtime.h"
#include "elastic_processor.h"
#include "render_plan.h"
#include "session/elastic.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace omawave::engine {
using session::FramePos;
struct ElasticRuntime::Slot {
    std::atomic<int> pins{0};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<FramePos> page{-1};
    std::array<float,pageFrames*2> audio{};
};
struct ElasticRuntime::Impl {
    struct State {
        std::array<Slot,pages> slots;
        // Single audio producer. Odd epoch marks an origin update in progress.
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<FramePos> origin{-1}, wanted{-1};
        FramePos last = -1, end = -1; // audio only
        std::unique_ptr<ElasticProcessor> processor; // worker only
        std::uint64_t renderedEpoch = 0;
        FramePos next = -1;
    };
    std::vector<Voice> voices;
    std::uint32_t rate;
    std::unique_ptr<State[]> state;
    std::atomic<bool> stop{false}, error{false};
    std::thread worker;
    Impl(std::vector<Voice> input, std::uint32_t sampleRate)
        : voices(std::move(input)), rate(sampleRate) {
        if (voices.size() > maxVoices) throw std::runtime_error("at most 64 elastic clip runs are supported");
        state = std::make_unique<State[]>(voices.size());
        worker = std::thread([this] { work(); });
    }
    ~Impl() { stop.store(true,std::memory_order_release); if (worker.joinable()) worker.join(); }
    bool step(std::size_t v) {
        auto& s = state[v]; const auto& voice = voices[v];
        const auto epoch = s.epoch.load(std::memory_order_acquire);
        if (!epoch || epoch%2) return false;
        const auto origin = s.origin.load(std::memory_order_relaxed), wanted = s.wanted.load(std::memory_order_acquire);
        if (s.epoch.load(std::memory_order_acquire) != epoch || origin < 0) return false;
        if (s.renderedEpoch != epoch) {
            s.processor = std::make_unique<ElasticProcessor>(voice.source->audio,voice.spec,rate);
            s.next = origin; s.renderedEpoch = epoch;
        }
        // Keep three pages ahead, never enough to overwrite a requested page.
        if (s.next > wanted+3 || s.next > (voice.spec.outputFrames-1)/FramePos(pageFrames)) return false;
        auto& slot = s.slots[std::size_t(s.next)%pages];
        int zero = 0;
        if (!slot.pins.compare_exchange_strong(zero,-1,std::memory_order_acquire)) return false;
        try {
            const auto start = s.next*FramePos(pageFrames);
            const auto count = std::uint32_t(std::min<FramePos>(pageFrames,voice.spec.outputFrames-start));
            float* planes[] = {slot.audio.data(),slot.audio.data()+pageFrames};
            s.processor->render(start,count,planes,&stop);
            for (unsigned c = 0; c < 2; ++c) std::fill(planes[c]+count,planes[c]+pageFrames,0);
            slot.page.store(s.next,std::memory_order_relaxed);
            slot.epoch.store(epoch,std::memory_order_relaxed);
            slot.pins.store(0,std::memory_order_release);
            ++s.next;
        } catch (...) { slot.epoch.store(0,std::memory_order_relaxed); slot.pins.store(0,std::memory_order_release); throw; }
        return true;
    }
    void work() noexcept {
        try {
            while (!stop.load(std::memory_order_acquire)) {
                bool progress = false;
                for (std::size_t v = 0; v < voices.size() && !stop.load(std::memory_order_relaxed); ++v) progress = step(v) || progress;
                if (!progress) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (...) { if (!stop.load(std::memory_order_relaxed)) error.store(true,std::memory_order_release); }
    }
};
ElasticRuntime::ElasticRuntime(std::vector<Voice> voices, std::uint32_t rate)
    : impl_(std::make_unique<Impl>(std::move(voices),rate)) {}
ElasticRuntime::~ElasticRuntime() = default;
bool ElasticRuntime::matches(const std::vector<Voice>& voices, std::uint32_t rate) const { return impl_->rate == rate && impl_->voices == voices; }
bool ElasticRuntime::failed() const noexcept { return impl_->error.load(std::memory_order_acquire); }
ElasticRuntime::Lease::~Lease() { if (slot_) slot_->pins.fetch_sub(1,std::memory_order_release); }
ElasticRuntime::Lease::Lease(Lease&& other) noexcept : slot_(std::exchange(other.slot_,nullptr)) {}
ElasticRuntime::Lease& ElasticRuntime::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) { if (slot_) slot_->pins.fetch_sub(1,std::memory_order_release); slot_ = std::exchange(other.slot_,nullptr); }
    return *this;
}
float ElasticRuntime::Lease::at(unsigned channel, std::size_t frame) const noexcept {
    return slot_ && channel < 2 && frame < pageFrames ? slot_->audio[channel*pageFrames+frame] : 0;
}
ElasticRuntime::Lease ElasticRuntime::read(std::size_t voice, std::size_t page) noexcept {
    auto& s = impl_->state[voice]; auto& slot = s.slots[page%pages];
    const auto epoch = s.epoch.load(std::memory_order_acquire);
    auto pins = slot.pins.load(std::memory_order_relaxed);
    // Bounded CAS: only this callback reads; one worker can contend for a slot.
    for (unsigned n = 0; n < 3 && pins >= 0; ++n) {
        if (!slot.pins.compare_exchange_weak(pins,pins+1,std::memory_order_acquire)) continue;
        if (slot.epoch.load(std::memory_order_relaxed) == epoch && epoch && !(epoch%2)
            && slot.page.load(std::memory_order_relaxed) == FramePos(page)) return Lease(&slot);
        slot.pins.fetch_sub(1,std::memory_order_release); break;
    }
    return {};
}
bool ElasticRuntime::prepare(std::size_t voice, FramePos start, std::uint32_t frames) noexcept {
    if (!frames) return true;
    if (voice >= impl_->voices.size() || start < 0 || start > impl_->voices[voice].spec.outputFrames
        || frames > std::uint64_t(impl_->voices[voice].spec.outputFrames-start) || frames > pageFrames) return false;
    auto& s = impl_->state[voice];
    const auto page = start/FramePos(pageFrames), last = (start+frames-1)/FramePos(pageFrames);
    if (start != s.last && start != s.end) {
        s.epoch.fetch_add(1,std::memory_order_acq_rel);
        s.origin.store(page,std::memory_order_relaxed); s.wanted.store(last,std::memory_order_relaxed);
        s.epoch.fetch_add(1,std::memory_order_release);
    } else s.wanted.store(last,std::memory_order_release);
    s.last = start; s.end = start+frames;
    bool ready = true;
    for (auto p = page; p <= last; ++p) { auto lease = read(voice,std::size_t(p)); ready = bool(lease) && ready; }
    if (!ready) preparing_.store(true,std::memory_order_relaxed);
    return ready;
}
void prepareElastic(RenderPlan& plan, const RenderPlan* previous) {
    std::vector<ElasticRuntime::Voice> voices;
    for (auto& track : plan.tracks) {
        PlanClip* prior = nullptr;
        for (auto& clip : track.clips) {
            clip.elasticRuntime = nullptr;
            if (!clip.elastic) { prior = nullptr; continue; }
            if (prior && prior->owner == clip.owner && prior->elastic == clip.elastic
                && prior->timelineEnd() == clip.timelineStart && prior->sourceOffset+prior->length == clip.sourceOffset) {
                clip.elasticVoice = prior->elasticVoice; voices.back().length += clip.length;
            } else {
                clip.elasticVoice = voices.size();
                voices.push_back({clip.owner,*clip.elastic,clip.timelineStart,clip.sourceOffset,clip.length});
            }
            prior = &clip;
        }
    }
    if (voices.empty()) { plan.elasticRuntime.reset(); return; }
    if (!plan.elasticRuntime || !plan.elasticRuntime->matches(voices,plan.sampleRate)) {
        if (previous && previous->elasticRuntime && previous->elasticRuntime->matches(voices,plan.sampleRate)) plan.elasticRuntime = previous->elasticRuntime;
        else plan.elasticRuntime = std::make_shared<ElasticRuntime>(voices,plan.sampleRate);
    }
    for (auto& track : plan.tracks) for (auto& clip : track.clips) if (clip.elastic) clip.elasticRuntime = plan.elasticRuntime.get();
}
bool prepareElasticTrack(const PlanTrack& track, FramePos start, std::uint32_t frames) noexcept {
    const auto end = start > std::numeric_limits<FramePos>::max()-frames ? std::numeric_limits<FramePos>::max() : start+frames;
    bool ready = true;
    for (auto i = track.indexAt(start); i < track.clips.size();) {
        const auto& clip = track.clips[i++]; if (clip.timelineStart >= end) break;
        if (!clip.elasticRuntime) continue;
        const auto from = std::max(start,clip.timelineStart);
        auto to = std::min(end,clip.timelineEnd());
        // Request a contiguous split run once; retries must keep the same epoch.
        while (i < track.clips.size() && track.clips[i].elasticRuntime == clip.elasticRuntime
            && track.clips[i].elasticVoice == clip.elasticVoice && track.clips[i].timelineStart < end) {
            to = std::min(end,track.clips[i].timelineEnd()); ++i;
        }
        ready = clip.elasticRuntime->prepare(clip.elasticVoice,clip.sourceOffset+from-clip.timelineStart,std::uint32_t(to-from)) && ready;
    }
    return ready;
}
bool prepareElasticBlock(const RenderPlan& plan, FramePos start, std::uint32_t frames) noexcept {
    bool ready = true;
    for (const auto& track : plan.tracks) ready = prepareElasticTrack(track,start,frames) && ready;
    return ready;
}
} // namespace omawave::engine
