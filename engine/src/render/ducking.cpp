#include "ducking.h"

#include "session/edits.h"
#include "engine/routing_render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace omawave::render {
namespace {
using namespace session;
constexpr auto maximum = std::numeric_limits<FramePos>::max();
struct Cancelled {};
void check(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}
void require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
void require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
FramePos plusClamped(FramePos frame, FramePos amount) { return frame > maximum-amount ? maximum : frame+amount; }
struct Region { FramePos attack, onset, held, released; };

// Runs are merged before points are emitted: a brief gap cannot create two
// opposing ramps or a one-window volume bump between nearby trigger passages.
void appendRegion(std::vector<Region>& regions, Region region) {
    if (!regions.empty() && region.attack <= regions.back().released) {
        auto& last=regions.back();
        last.attack=std::min(last.attack,region.attack); last.onset=std::min(last.onset,region.onset);
        last.held=std::max(last.held,region.held); last.released=std::max(last.released,region.released);
    } else {
        require(regions.size() < kMaxProjectAutomationPoints,
                "ducking analysis exceeds the supported region density; increase Hold or choose a shorter range");
        regions.push_back(region);
    }
}

class Detector {
public:
    Detector(std::uint32_t rate, double threshold, FramePos attack, FramePos hold, FramePos release,
             FramePos first, FramePos last, const std::atomic<bool>* cancel)
        : window_(std::max(1u,rate/100)), threshold_(threshold), attack_(attack), hold_(hold), release_(release),
          first_(first),last_(last),cancel_(cancel) {}
    void process(FramePos at, const float* const* planes, std::uint32_t channels, std::uint32_t frames) {
        require(at==next_ && channels>0 && channels<=power_.size(), "ducking received invalid analysis blocks");
        for (std::uint32_t f=0;f<frames;++f) {
            if ((f&1023)==0) check(cancel_);
            for (std::uint32_t c=0;c<channels;++c) power_[c]+=double(planes[c][f])*planes[c][f];
            ++filled_; ++next_;
            if (filled_==window_) complete();
        }
    }
    std::vector<Region> finish() {
        if (filled_) complete();
        if (active_) close(next_);
        return std::move(regions_);
    }
private:
    void complete() {
        const double rmsPower=*std::max_element(power_.begin(),power_.end())/filled_;
        require(std::isfinite(rmsPower), "ducking trigger contains invalid sample power");
        const bool audible=rmsPower>=threshold_;
        if (audible && !active_) { active_=true; onset_=next_-filled_; }
        else if (!audible && active_) close(next_-filled_);
        power_.fill(0); filled_=0;
    }
    void close(FramePos end) {
        active_=false;
        Region region{std::max(FramePos{0},onset_-attack_),onset_,plusClamped(end,hold_),0};
        region.released=plusClamped(region.held,release_);
        if (region.attack<last_ && region.released>first_) appendRegion(regions_,region);
    }
    std::uint32_t window_=0,filled_=0;
    double threshold_;
    FramePos attack_,hold_,release_,first_,last_,next_=0,onset_=0;
    const std::atomic<bool>* cancel_;
    bool active_=false;
    std::array<double,2> power_{};
    std::vector<Region> regions_;
};

bool needsReplacement(const AutomationCurve& curve, FramePos start, FramePos end) {
    for (const auto& point:curve.points) if (point.frame>=start && point.frame<end) return true;
    return curve.active() && (curve.preciseValueAt(start,1)!=1 || curve.preciseValueAt(end-1,1)!=1);
}

AutomationPoint atFrame(const std::vector<Region>& regions, FramePos frame, double level) {
    const auto it=std::lower_bound(regions.begin(),regions.end(),frame,
        [](const Region& region,FramePos f) { return region.released<=f; });
    if (it==regions.end() || frame<it->attack) return {frame,1,AutomationInterpolation::Hold};
    if (frame<it->onset)
        return {frame,std::lerp(1.0,level,double(frame-it->attack)/(it->onset-it->attack)),AutomationInterpolation::Linear};
    if (frame<it->held) return {frame,level,AutomationInterpolation::Hold};
    return {frame,std::lerp(level,1.0,double(frame-it->held)/(it->released-it->held)),AutomationInterpolation::Linear};
}

AutomationCurve makeCurve(const AutomationCurve& original,const std::vector<Region>& regions,
                          const DuckingSettings& spec,double level) {
    using Mode=AutomationInterpolation;
    std::vector<AutomationPoint> generated;
    const auto add=[&](AutomationPoint point) {
        if (point.frame<spec.start || point.frame>=spec.end) return;
        if (!generated.empty() && generated.back().frame==point.frame) generated.back()=point;
        else { require(generated.size()<AutomationCurve::kMaxPoints,"generated ducking exceeds 4096 points; increase Hold or choose a shorter range"); generated.push_back(point); }
    };
    add(atFrame(regions,spec.start,level));
    for (const auto& region:regions) {
        add({region.attack,1,Mode::Linear});
        add({region.onset,level,Mode::Hold});
        add({region.held,level,Mode::Linear});
        add({region.released,1,Mode::Hold});
    }
    add(atFrame(regions,spec.end-1,level));
    AutomationCurve result=original;
    require(result.replaceInterval(spec.start,spec.end-1,generated,1),
        "ducking cannot preserve the existing automation shape or exceeds 4096 points; choose another range");
    require(result.valid(0,4),"generated ducking has invalid automation geometry");
    return result;
}
} // namespace

DuckingResult generateDucking(const session::Project& project, session::TrackId target,
    const std::vector<session::TrackId>& triggers, const DuckingSettings& spec,
    const std::vector<std::vector<TrackChain>>& triggerChains,
    const std::atomic<bool>* cancel, const ProgressFn& progress)
{
    DuckingResult result;
    try {
        check(cancel);
        const auto* destination=project.track(target);
        require(destination,"choose a destination track for ducking"); result.curve=destination->volumeAutomation;
        require(project.sampleRate()>=8000 && project.sampleRate()<=192000,"ducking needs a supported project sample rate");
        for (const auto& track:project.tracks()) {
            require(edits::isWellFormed(track.clips),"ducking requires valid clip geometry");
            for (const auto& clip:track.clips) {
                const auto* source=project.source(clip.source);
                require(source && source->sampleRate==project.sampleRate() && source->audio.channels()>0
                    && source->audio.frames()<=std::uint64_t(maximum) && clip.validSource(FramePos(source->audio.frames())),
                    "ducking requires valid source intervals");
            }
        }
        require(spec.start>=0 && spec.end>spec.start && spec.end<=project.length(),"choose a nonempty ducking range inside the project");
        require(std::isfinite(spec.thresholdDb) && spec.thresholdDb>=-80 && spec.thresholdDb<=0
            && std::isfinite(spec.reductionDb) && spec.reductionDb>=0 && spec.reductionDb<=60,
            "choose a threshold from -80 to 0 dBFS and a reduction from 0 to 60 dB");
        const auto duration=[&](double seconds) {
            require(std::isfinite(seconds) && seconds>=0 && seconds<=10,"ducking attack, hold and release must be from 0 to 10 seconds");
            return FramePos(std::llround(seconds*project.sampleRate()));
        };
        const auto attack=duration(spec.attackSeconds),hold=duration(spec.holdSeconds),release=duration(spec.releaseSeconds);
        const auto& original=destination->volumeAutomation;
        require(original.valid(0,4),"the destination volume automation is invalid");
        require(original.enabled || original.points.empty(),"enable or clear the destination's disabled volume automation before generating ducking");
        require(project.automationPointCount()<=kMaxProjectAutomationPoints,"the project exceeds its automation point budget");
        require(!triggers.empty(),"choose at least one ducking trigger track");
        require(triggers.size()<=engine::RoutingRender::maxTracks,"choose at most "+std::to_string(engine::RoutingRender::maxTracks)+" ducking trigger tracks");
        require(triggerChains.empty() || triggerChains.size()==triggers.size(),"supply a separate processor group for each trigger");
        std::set<TrackId> seen;
        std::set<const lv2::Chain*> usedProcessors;
        std::vector<Project> isolated; isolated.reserve(triggers.size());
        for (std::size_t i=0;i<triggers.size();++i) {
            require(triggers[i]!=target && seen.insert(triggers[i]).second,"choose distinct trigger tracks other than the destination");
            std::string error; auto source=isolateTrackOutput(project,triggers[i],error);
            if (!source) throw std::runtime_error(error);
            require(source->tracks().size()<=engine::RoutingRender::maxTracks,
                    "a trigger's incoming routing exceeds the supported "+std::to_string(engine::RoutingRender::maxTracks)+" tracks");
            require(!source->track(target),"the destination feeds a trigger; choose an independent trigger track");
            const std::vector<TrackChain> empty; const auto& chains=triggerChains.empty()?empty:triggerChains[i];
            std::set<TrackId> supplied;
            for (const auto& chain:chains) {
                require(chain.chain && source->track(chain.track) && supplied.insert(chain.track).second,
                        "a trigger has an invalid or duplicate processor chain");
                require(usedProcessors.insert(chain.chain).second,"ducking trigger passes need independently prepared processors");
            }
            for (const auto& track:source->tracks()) for (std::size_t slot=0;slot<track.inserts.size();++slot) {
                if (track.inserts[slot].bypass) continue;
                const auto found=std::find_if(chains.begin(),chains.end(),[&](const auto& chain) { return chain.track==track.id; });
                require(found!=chains.end() && found->chain->hasProcessor(slot) && !found->chain->bypassed(slot),
                        "an active trigger effect is unavailable; ducking analysis was not run");
            }
            isolated.push_back(std::move(*source));
        }
        if (spec.reductionDb==0) return result;
        std::vector<Region> regions;
        for (std::size_t i=0;i<isolated.size();++i) {
            check(cancel);
            auto end=std::min(project.length(),plusClamped(spec.end,attack));
            // Complete the final detector window. Truncating it at the selected
            // endpoint would change its RMS and make identical audio trigger
            // differently when only the selection boundary moves.
            const auto window=FramePos(std::max(1u,project.sampleRate()/100));
            if (const auto remainder=end%window; remainder)
                end=std::min(project.length(),plusClamped(end,window-remainder));
            Detector detector(project.sampleRate(),std::pow(10.0,spec.thresholdDb/10),attack,hold,release,spec.start,spec.end,cancel);
            const std::vector<TrackChain> empty; const auto& chains=triggerChains.empty()?empty:triggerChains[i];
            const auto streamed=visitMixInput(isolated[i],2,end,
                [&](FramePos at,const float* const* planes,std::uint32_t channels,std::uint32_t frames) { detector.process(at,planes,channels,frames); },
                chains,cancel,[&](float fraction) { if (progress) progress(float((i+double(fraction))/isolated.size())); });
            if (streamed.cancelled) throw Cancelled{};
            if (!streamed.ok) throw std::runtime_error(streamed.error);
            require(result.analyzedFrames<=maximum-streamed.frames,"ducking analysis duration exceeds its supported total");
            result.analyzedFrames+=streamed.frames;
            auto found=detector.finish();
            require(regions.size()+found.size()<=kMaxProjectAutomationPoints,
                    "ducking analysis exceeds the supported region density; increase Hold or choose a shorter range");
            regions.insert(regions.end(),found.begin(),found.end());
        }
        check(cancel);
        if (progress) progress(1);
        check(cancel);
        if (regions.empty()) return result;
        std::sort(regions.begin(),regions.end(),[](const auto& a,const auto& b) { return a.attack<b.attack; });
        std::vector<Region> merged; for (const auto& region:regions) appendRegion(merged,region);
        result.regions=merged.size();
        if (!spec.replaceInRange && needsReplacement(original,spec.start,spec.end)) {
            result.replacementRequired=true;
            result.error="this range contains manual volume automation; enable Replace in range to generate ducking"; return result;
        }
        auto curve=makeCurve(original,merged,spec,std::pow(10.0,-spec.reductionDb/20));
        require(project.automationPointCount()-original.points.size()+curve.points.size()<=kMaxProjectAutomationPoints,
                "ducking would exceed the project automation point budget");
        check(cancel); result.changed=curve!=original; result.curve=std::move(curve); return result;
    } catch (const Cancelled&) { result.cancelled=true; result.changed=false; result.error.clear(); return result; }
      catch (const std::exception& error) {
        result.changed=false; result.cancelled=cancel && cancel->load(std::memory_order_relaxed);
        result.error=result.cancelled?std::string{}:error.what(); return result;
    }
}
} // namespace omawave::render
