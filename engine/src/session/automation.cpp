#include "automation.h"
#include "track.h"
#include "project.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace omawave::session {
namespace {
const std::array<PanGains,4097> panTable = [] {
    std::array<PanGains,4097> result{};
    for (std::size_t i=0;i<result.size();++i)
        result[i]=panGainsFor(static_cast<float>(i)/2048.0f-1.0f);
    return result;
}();
}
PanGains automationPan(float position) noexcept
{
    if (!std::isfinite(position)) position=0;
    const float at=(std::clamp(position,-1.0f,1.0f)+1.0f)*2048.0f;
    const auto index=static_cast<std::size_t>(at);
    if (index>=4096) return panTable.back();
    const float t=at-static_cast<float>(index);
    const auto& a=panTable[index]; const auto& b=panTable[index+1];
    return {a.monoL+(b.monoL-a.monoL)*t,a.monoR+(b.monoR-a.monoR)*t,
            a.stereoL+(b.stereoL-a.stereoL)*t,a.stereoR+(b.stereoR-a.stereoR)*t};
}
bool AutomationCurve::valid(double minimum,double maximum) const noexcept
{
    if (points.size()>kMaxPoints) return false;
    FramePos previous=-1;
    for (const auto& point:points) {
        if (point.frame<=previous || !std::isfinite(point.value) || point.value<minimum || point.value>maximum
            || (point.interpolation != AutomationInterpolation::Default && point.interpolation != AutomationInterpolation::Linear
                && point.interpolation != AutomationInterpolation::Hold && point.interpolation != AutomationInterpolation::Smooth)
            || !std::isfinite(point.smoothStart) || !std::isfinite(point.smoothEnd)
            || point.smoothStart < 0 || point.smoothEnd > 1 || point.smoothStart >= point.smoothEnd
            || (point.interpolation != AutomationInterpolation::Smooth && (point.smoothStart != 0 || point.smoothEnd != 1))) return false;
        previous=point.frame;
    }
    return true;
}
std::string validateAutomation(const std::vector<Insert>& inserts)
{
    for(const auto& insert:inserts){
        const bool vst=insert.uri.starts_with("vst3:");
        const double limit=insert.uri.starts_with("clap:")?std::numeric_limits<double>::max():std::numeric_limits<float>::max();
        const double minimum=vst?0:-limit,maximum=vst?1:limit;
        const auto baseValid=[&](double value){return std::isfinite(value) && value>=minimum && value<=maximum;};
        for(const auto& control:insert.controls){
            if(!baseValid(control.value) || !control.automation.valid(minimum,maximum))
                return "an effect has invalid automation or a non-finite/out-of-range static value";
            for(const auto& instance:control.instanceAutomation)if(instance)
                if(!baseValid(instance->base) || !instance->curve.valid(minimum,maximum))
                    return "an effect instance has invalid automation or a non-finite/out-of-range static base";
        }
    }
    return {};
}
std::string validateAutomation(const Track& track)
{
    if(!track.volumeAutomation.valid(0,4) || !track.panAutomation.valid(-1,1))
        return "a track has invalid volume or pan automation";
    return validateAutomation(track.inserts);
}
std::string validateAutomation(const Project& project)
{
    if(project.automationPointCount()>kMaxProjectAutomationPoints)
        return "automation exceeds the project limit of 65536 points";
    if(!project.masterVolumeAutomation().valid(0,4)) return "the master has invalid volume automation";
    if(const auto error=validateAutomation(project.masterInserts());!error.empty()) return error;
    for(const auto& track:project.tracks())
        if(const auto error=validateAutomation(track);!error.empty()) return error;
    return {};
}
double smoothAutomationFraction(double t, double start, double end) noexcept
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    if (start == 0 && end == 1) return t*t*(3-2*t);
    // Integral of the derivative's nonnegative Bernstein basis. Normalizing
    // first avoids subtraction cancellation and underflow in narrow windows.
    double q0=start*(1-start), q1=start*(1-end)+end*(1-start), q2=end*(1-end);
    const double scale=std::max({q0,q1,q2});
    q0/=scale; q1/=scale; q2/=scale;
    const double square=t*t;
    return std::clamp(t*(2*q0*(3-3*t+square)+q1*(3*t-2*square)+2*q2*square)
        /(2*q0+q1+2*q2),0.0,1.0);
}
double AutomationCurve::preciseValueAt(FramePos frame,double fallback) const noexcept
{
    if (!active()) return fallback;
    const auto next=std::upper_bound(points.begin(),points.end(),frame,
        [](FramePos f,const auto& p){return f<p.frame;});
    if (next==points.begin()) return next->value;
    const auto& left=*(next-1);
    if (next==points.end() || holds(left)) return left.value;
    const double t=static_cast<double>(frame-left.frame)/static_cast<double>(next->frame-left.frame);
    return std::lerp(left.value,next->value,left.interpolation == AutomationInterpolation::Smooth
        ? smoothAutomationFraction(t,left.smoothStart,left.smoothEnd) : t);
}
AutomationInterpolation AutomationCurve::interpolationAt(FramePos frame) const noexcept
{
    const auto next = std::upper_bound(points.begin(), points.end(), frame,
        [](FramePos f, const auto& p) { return f < p.frame; });
    return next == points.begin() ? AutomationInterpolation::Default : (next - 1)->interpolation;
}
bool AutomationCurve::hasRamp(FramePos start, FramePos end) const noexcept
{
    if (!active() || end <= start) return false;
    auto right = std::upper_bound(points.begin(), points.end(), start,
        [](FramePos f, const auto& p) { return f < p.frame; });
    if (right == points.begin()) ++right;
    for (; right != points.end() && (right - 1)->frame < end; ++right)
        if (!holds(*(right - 1)) && (right - 1)->value != right->value) return true;
    return false;
}
FramePos AutomationCurve::nextPoint(FramePos frame) const noexcept
{
    if (!active()) return std::numeric_limits<FramePos>::max();
    const auto next=std::upper_bound(points.begin(),points.end(),frame,[](FramePos f,const auto& p){return f<p.frame;});
    return next==points.end()?std::numeric_limits<FramePos>::max():next->frame;
}
FramePos AutomationCurve::controlFrame(FramePos frame,FramePos step) const noexcept
{
    frame=std::max(FramePos{0},frame);
    step=std::max(FramePos{1},step);
    const auto point=std::upper_bound(points.begin(),points.end(),frame,[](FramePos f,const auto& p){return f<p.frame;});
    return std::max(frame-frame%step,point==points.begin()?FramePos{0}:(point-1)->frame);
}
namespace {
using Mode=AutomationInterpolation;
constexpr double anyValue=std::numeric_limits<double>::max();
bool validPoint(const AutomationPoint& point)
{
    return point.frame>=0 && std::isfinite(point.value)
        && (point.interpolation==Mode::Default || point.interpolation==Mode::Linear || point.interpolation==Mode::Hold || point.interpolation==Mode::Smooth)
        && std::isfinite(point.smoothStart) && std::isfinite(point.smoothEnd)
        && point.smoothStart>=0 && point.smoothEnd<=1 && point.smoothStart<point.smoothEnd
        && (point.interpolation==Mode::Smooth || (point.smoothStart==0 && point.smoothEnd==1));
}
std::optional<double> phaseAt(const AutomationPoint& left, const AutomationPoint& right, FramePos frame)
{
    if(frame==left.frame) return left.smoothStart;
    if(frame==right.frame) return left.smoothEnd;
    const double phase=std::lerp(left.smoothStart,left.smoothEnd,
        double(frame-left.frame)/double(right.frame-left.frame));
    if(phase<=left.smoothStart || phase>=left.smoothEnd) return {};
    return phase;
}
std::optional<AutomationPoint> slicedPoint(const AutomationCurve& original, FramePos frame, double fallback)
{
    AutomationPoint result{frame,original.preciseValueAt(frame,fallback)};
    if(!original.active()) return result;
    const auto right=std::upper_bound(original.points.begin(),original.points.end(),frame,
        [](FramePos at,const auto& point){return at<point.frame;});
    if(right==original.points.begin()) return result;
    const auto& left=*(right-1);result.interpolation=left.interpolation;
    result.smoothStart=left.smoothStart;result.smoothEnd=left.smoothEnd;
    if(right!=original.points.end() && left.interpolation==Mode::Smooth){
        const auto phase=phaseAt(left,*right,frame);if(!phase) return {};
        result.smoothStart=*phase;
    }
    return result;
}
bool putBoundary(AutomationCurve& result,const AutomationCurve& original,FramePos frame,double fallback,bool incoming)
{
    const auto point=slicedPoint(original,frame,fallback);if(!point) return false;
    auto at=std::lower_bound(result.points.begin(),result.points.end(),frame,[](const auto& p,FramePos f){return p.frame<f;});
    if(incoming && at!=result.points.begin() && original.active()){
        auto& retained=*(at-1);
        const auto right=std::upper_bound(original.points.begin(),original.points.end(),retained.frame,
            [](FramePos f,const auto& p){return f<p.frame;});
        if(right!=original.points.begin() && right!=original.points.end() && (right-1)->interpolation==Mode::Smooth && frame<=right->frame){
            const auto first=phaseAt(*(right-1),*right,retained.frame),last=phaseAt(*(right-1),*right,frame);
            if(!first || !last || *first>=*last) return false;
            retained.smoothStart=*first;retained.smoothEnd=*last;
        }
    }
    if(at!=result.points.end() && at->frame==frame) *at=*point;
    else result.points.insert(at,*point);
    return true;
}
void compactConstants(AutomationCurve& curve)
{
    std::vector<AutomationPoint> compact;compact.reserve(curve.points.size());
    for(const auto& point:curve.points){
        while(compact.size()>=2 && compact.back().value==point.value && compact[compact.size()-2].value==point.value)
            compact.pop_back();
        compact.push_back(point);
    }
    curve.points=std::move(compact);
}
}
bool AutomationCurve::setPoint(FramePos frame,double value)
{
    if(frame<0 || !std::isfinite(value) || !valid(-anyValue,anyValue)) return false;
    const auto at=std::lower_bound(points.begin(),points.end(),frame,[](const auto& p,FramePos f){return p.frame<f;});
    if(at!=points.end() && at->frame==frame){
        if(at->value==value) return false;
        at->value=value;return true;
    }
    if(points.size()>=kMaxPoints) return false;
    if(at==points.begin() || (at-1)->interpolation!=Mode::Smooth){
        const auto interpolation=interpolationAt(frame);points.insert(at,{frame,value,interpolation});return true;
    }
    AutomationCurve result=*this,shape=*this;shape.enabled=true;
    if(!putBoundary(result,shape,frame,value,true)) return false;
    for(auto& point:result.points) if(point.frame==frame) point.value=value;
    *this=std::move(result);return true;
}
bool AutomationCurve::erasePoint(FramePos frame)
{
    const auto count=std::erase_if(points,[frame](const auto& p){return p.frame==frame;});
    if (points.empty()) enabled=false;
    return count>0;
}
bool AutomationCurve::removeTime(FramePos start,FramePos end)
{
    if(start<0 || end<=start || !valid(-anyValue,anyValue)) return false;
    if(points.empty()) return true;
    AutomationCurve shape=*this;shape.enabled=true;
    AutomationCurve result=*this;
    std::erase_if(result.points,[=](const auto& point){return point.frame>=start && point.frame<end;});
    if((start>0 && !putBoundary(result,shape,start-1,0,true)) || !putBoundary(result,shape,end,0,false)) return false;
    for(auto& point:result.points) if(point.frame>=end) point.frame-=end-start;
    if(!result.valid(-anyValue,anyValue)) return false;
    *this=std::move(result);return true;
}

bool AutomationCurve::writeControlInterval(FramePos start, FramePos end, double first, double last,
                                           double fallback, FramePos step)
{
    if (step <= 1) return writeInterval(start, end, first, last, fallback);
    if (start < 0 || end < start || end == std::numeric_limits<FramePos>::max()
        || !std::isfinite(first) || !std::isfinite(last) || !std::isfinite(fallback)
        || !valid(-anyValue,anyValue)) return false;
    AutomationCurve result = *this;
    if (!active()) result.points.clear();
    std::erase_if(result.points, [=](const auto& p) { return p.frame >= start && p.frame <= end; });
    const auto put = [&](FramePos frame, double value, AutomationInterpolation interpolation) {
        const auto at = std::lower_bound(result.points.begin(), result.points.end(), frame,
            [](const auto& p, FramePos f) { return p.frame < f; });
        if (at != result.points.end() && at->frame == frame) *at = {frame, value, interpolation};
        else result.points.insert(at, {frame, value, interpolation});
    };
    if (start > 0) {
        const auto before = controlFrame(start - 1, step);
        if(!putBoundary(result,*this,before,fallback,true)) return false;
        put(before, preciseValueAt(before, fallback), AutomationInterpolation::Hold);
    }
    const auto after = end + 1, sampledAfter = controlFrame(after, step);
    if(sampledAfter==after){if(!putBoundary(result,*this,after,fallback,false)) return false;}
    else put(after, preciseValueAt(sampledAfter, fallback), AutomationInterpolation::Hold);
    if (sampledAfter != after) {
        const auto remaining = step - after % step;
        const auto grid = after <= std::numeric_limits<FramePos>::max() - remaining
            ? after + remaining : std::numeric_limits<FramePos>::max();
        const auto next = std::min(grid, nextPoint(after));
        if (next != std::numeric_limits<FramePos>::max() && !putBoundary(result,*this,next,fallback,false)) return false;
    }
    const auto lastGrid = std::max(start, end - end % step);
    put(start, first, AutomationInterpolation::Linear);
    put(lastGrid, end == start ? first : std::lerp(first, last, double(lastGrid - start) / (end - start)),
        AutomationInterpolation::Hold);
    compactConstants(result);
    if (!result.valid(-anyValue,anyValue)) return false;
    result.enabled = true;
    *this = std::move(result); return true;
}

bool AutomationCurve::replaceInterval(FramePos start,FramePos end,const std::vector<AutomationPoint>& replacement,double fallback,bool compact)
{
    if(start<0 || end<start || end==std::numeric_limits<FramePos>::max() || !std::isfinite(fallback)
        || !valid(-anyValue,anyValue) || replacement.empty() || replacement.size()>kMaxPoints
        || replacement.front().frame!=start || replacement.back().frame!=end) return false;
    FramePos previous=-1;
    for(const auto& point:replacement){if(!validPoint(point) || point.frame<=previous) return false;previous=point.frame;}
    AutomationCurve result=*this;
    if(!active()) result.points.clear();
    std::erase_if(result.points,[=](const auto& point){return point.frame>=start && point.frame<=end;});
    if((start>0 && !putBoundary(result,*this,start-1,fallback,true)) || !putBoundary(result,*this,end+1,fallback,false)) return false;
    const auto at=std::lower_bound(result.points.begin(),result.points.end(),start,[](const auto& p,FramePos f){return p.frame<f;});
    result.points.insert(at,replacement.begin(),replacement.end());
    if(compact) compactConstants(result);
    result.enabled=true;
    if(!result.valid(-anyValue,anyValue)) return false;
    *this=std::move(result);return true;
}
bool AutomationCurve::writeInterval(FramePos start,FramePos end,double first,double last,double fallback)
{
    if(!std::isfinite(first) || !std::isfinite(last)) return false;
    std::vector<AutomationPoint> replacement{{start,end==start?last:first,end!=start && first!=last && hold?Mode::Linear:Mode::Default}};
    if(end!=start) replacement.push_back({end,last});
    return replaceInterval(start,end,replacement,fallback,true);
}
} // namespace omawave::session
