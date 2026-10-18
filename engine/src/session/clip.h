#pragma once

#include "types.h"
#include "fade_curve.h"
#include "automation.h"
#include "clip_identity.h"
#include "effect_owner.h"
#include "insert.h"
#include <limits>
#include <optional>
#include <cmath>

namespace omawave::session {

// An immutable transform of an original source interval. sourceOffset/length
// select the virtual output, so splitting never changes the transform itself.
struct ElasticSpec {
    FramePos inputStart = 0, inputFrames = 0, outputFrames = 0;
    float semitones = 0;
    [[nodiscard]] bool valid() const noexcept {
        return inputStart >= 0 && inputFrames > 0 && outputFrames > 0
            && outputFrames >= inputFrames/2 + inputFrames%2
            && (outputFrames <= inputFrames || outputFrames-inputFrames <= inputFrames)
            && std::isfinite(semitones) && semitones >= -12 && semitones <= 12;
    }
    bool operator==(const ElasticSpec&) const = default;
};

// Immutable once attached to a Clip. Adjacent split views share this logical
// processing region and one live processor; snapshots retain older values.
// The dry origin can extend outside the source: hidden context is zero padded,
// while every visible Clip must still pass validSource().
struct ClipRack {
    EffectOwner owner = EffectOwner::clip(freshClipToken());
    std::vector<Insert> inserts{};
    SourceId source = kInvalidSource;
    FramePos sourceOffset = 0;
    FramePos length = 0;
    std::optional<ElasticSpec> elastic{};

    [[nodiscard]] bool validOrigin() const noexcept {
        return owner.isClip() && source != kInvalidSource && length > 0
            && (sourceOffset <= 0 || length <= std::numeric_limits<FramePos>::max() - sourceOffset)
            && (!elastic || elastic->valid());
    }
    // Persistent content, not session-local membership or edit provenance.
    [[nodiscard]] bool operator==(const ClipRack& other) const noexcept {
        return inserts == other.inserts && source == other.source
            && sourceOffset == other.sourceOffset && length == other.length && elastic == other.elastic;
    }
};

inline bool sameInsertDescriptors(const std::vector<Insert>& a, const std::vector<Insert>& b) noexcept
{
    if (a != b) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].identity != b[i].identity || a[i].settingsRevision != b[i].settingsRevision) return false;
        for (std::size_t j = 0; j < a[i].controls.size(); ++j)
            if (a[i].controls[j].revision != b[i].controls[j].revision) return false;
    }
    return true;
}

// A view onto imported audio, placed on the timeline.
//
// Edits rewrite clips, never audio. Cutting ten minutes out of a take copies no
// samples at all -- it edits a handful of these. That is what makes undo
// command-based rather than snapshot-based (BRIEF.md section 5).
//
// Gain and fades are properties applied at render time, not destructive writes, for
// the same reason: they must be reversible without keeping a copy of the audio.
struct Clip {
    SourceId source = kInvalidSource;

    // Where the clip starts on the timeline.
    FramePos timelineStart = 0;

    // Offset into the source where this clip begins reading.
    FramePos sourceOffset = 0;

    // How much of the source the clip covers.
    FramePos length = 0;

    // Linear, not dB. Conversion happens at the UI boundary so the audio path never
    // runs a std::pow per block.
    float gain = 1.0f;

    // Original ramp lengths in frames. A split keeps these denominators and
    // advances the offsets instead of restarting or steepening either ramp.
    FramePos fadeIn = 0;
    FramePos fadeOut = 0;
    FramePos fadeInOffset = 0;   // elapsed ramp frames before this clip's start
    FramePos fadeOutOffset = 0;  // ramp frames remaining after this clip's end
    std::optional<ElasticSpec> elastic{};
    // Editing membership only; never changes the audio or its render envelope.
    // Copies and split fragments retain membership until explicitly regrouped.
    ClipGroupId group = kInvalidClipGroup;
    FadeShape fadeInShape = FadeShape::Linear, fadeOutShape = FadeShape::Linear;
    float fadeInCurve = 0, fadeOutCurve = 0;
    // Coordinates belong to the clip, independently of source/slip and timeline.
    // Split views keep every knot and advance this offset, preserving samples.
    AutomationCurve volumeEnvelope{}, panEnvelope{};
    FramePos envelopeOffset = 0;
    ClipIdentity identity{};
    std::shared_ptr<const ClipRack> rack{};
    FramePos effectOffset = 0;

    void forkIdentity() { identity = ClipIdentity{}; }
    [[nodiscard]] bool hasValidEffects() const noexcept {
        if (!rack) return effectOffset == 0;
        return rack->validOrigin() && source == rack->source && elastic == rack->elastic
            && sourceOffset >= 0 && effectOffset >= 0 && length > 0
            && effectOffset <= rack->length && length <= rack->length - effectOffset
            && rack->sourceOffset == sourceOffset - effectOffset;
    }

    [[nodiscard]] bool hasEnvelopes() const noexcept {
        return volumeEnvelope.hasData() || panEnvelope.hasData() || envelopeOffset != 0;
    }
    [[nodiscard]] bool hasValidEnvelopes() const noexcept {
        return envelopeOffset >= 0 && length >= 0
            && envelopeOffset <= std::numeric_limits<FramePos>::max() - length
            && volumeEnvelope.valid(0,4) && panEnvelope.valid(-1,1);
    }

    [[nodiscard]] FramePos sourceFrames(FramePos physicalFrames) const noexcept {
        return elastic ? elastic->outputFrames : physicalFrames;
    }
    [[nodiscard]] bool validSource(FramePos physicalFrames) const noexcept {
        if (physicalFrames < 0 || sourceOffset < 0 || length <= 0) return false;
        if (elastic && (!elastic->valid() || elastic->inputStart > physicalFrames
            || elastic->inputFrames > physicalFrames - elastic->inputStart)) return false;
        const auto frames = sourceFrames(physicalFrames);
        return sourceOffset <= frames && length <= frames - sourceOffset;
    }

    [[nodiscard]] constexpr bool hasValidFades() const noexcept
    {
        return fadeIn >= 0 && fadeOut >= 0 && fadeInOffset >= 0 && fadeOutOffset >= 0
            && (fadeIn == 0 ? fadeInOffset == 0 : fadeInOffset < fadeIn)
            && (fadeOut == 0 ? fadeOutOffset == 0 : fadeOutOffset < fadeOut)
            && validFadeCurve(fadeInShape, fadeInCurve) && validFadeCurve(fadeOutShape, fadeOutCurve);
    }

    [[nodiscard]] constexpr bool hasPartialFades() const noexcept
    {
        return fadeInOffset != 0 || fadeOutOffset != 0 || fadeIn > length || fadeOut > length;
    }

    // Needed so a SessionState can compare equal to another, which is how the undo
    // stack refuses to record an edit that changed nothing.
    [[nodiscard]] bool operator==(const Clip& other) const noexcept {
        return group == other.group && sameAudio(other);
    }

    // Accepted edits need the same view, logical rack/inserts and exact current
    // settings provenance, even when a replacement has equal persistent values.
    [[nodiscard]] bool sameDescriptor(const Clip& other) const noexcept {
        return identity == other.identity && *this == other
            && (!rack || (rack->owner == other.rack->owner
                && sameInsertDescriptors(rack->inserts, other.rack->inserts)));
    }

    // Editing identity is irrelevant when comparing audio or a stored recipe.
    [[nodiscard]] bool sameAudio(const Clip& other) const noexcept
    {
        return source == other.source && timelineStart == other.timelineStart
            && sourceOffset == other.sourceOffset && length == other.length && gain == other.gain
            && fadeIn == other.fadeIn && fadeOut == other.fadeOut
            && fadeInOffset == other.fadeInOffset && fadeOutOffset == other.fadeOutOffset
            && elastic == other.elastic && fadeInShape == other.fadeInShape && fadeOutShape == other.fadeOutShape
            && fadeInCurve == other.fadeInCurve && fadeOutCurve == other.fadeOutCurve
            && volumeEnvelope == other.volumeEnvelope && panEnvelope == other.panEnvelope
            && envelopeOffset == other.envelopeOffset && effectOffset == other.effectOffset
            && bool(rack) == bool(other.rack) && (!rack || *rack == *other.rack);
    }

    [[nodiscard]] constexpr FramePos timelineEnd() const noexcept
    {
        return timelineStart + length;
    }

    [[nodiscard]] constexpr bool covers(FramePos pos) const noexcept
    {
        return pos >= timelineStart && pos < timelineEnd();
    }

    [[nodiscard]] constexpr bool intersects(FramePos start, FramePos end) const noexcept
    {
        return timelineStart < end && start < timelineEnd();
    }

    // Gain envelope at a frame offset from the clip's start, fades included.
    [[nodiscard]] float envelopeAt(FramePos offsetInClip) const noexcept
    {
        if (offsetInClip < 0 || offsetInClip >= length)
            return 0.0f;

        float envelope = gain;

        if (fadeIn > 0 && offsetInClip < fadeIn - fadeInOffset) {
            envelope *= fadeCurveAt(static_cast<float>(offsetInClip + fadeInOffset)
                      / static_cast<float>(fadeIn), fadeInShape, fadeInCurve);
        }
        if (fadeOut > 0) {
            const FramePos fromEnd = length - offsetInClip;
            if (fromEnd <= fadeOut - fadeOutOffset)
                envelope *= fadeCurveAt(static_cast<float>(fromEnd + fadeOutOffset)
                          / static_cast<float>(fadeOut), fadeOutShape, fadeOutCurve);
        }
        if (volumeEnvelope.active()) envelope *= volumeEnvelope.valueAt(envelopeOffset + offsetInClip,1);
        return envelope;
    }

    // A stereo-source balance precedes track effects and its independent pan.
    // Mono sources and channels beyond the stereo pair retain their exact level.
    [[nodiscard]] float channelEnvelopeAt(FramePos local, std::uint32_t channel,
                                           std::uint32_t sourceChannels) const noexcept {
        auto value = envelopeAt(local);
        if (local >= 0 && local < length && sourceChannels >= 2 && channel < 2 && panEnvelope.active()) {
            const auto pan = panEnvelope.valueAt(envelopeOffset + local,0);
            value *= channel == 0 ? (pan > 0 ? 1-pan : 1) : (pan < 0 ? 1+pan : 1);
        }
        return value;
    }

    // Splits into two clips at an absolute timeline position. The caller must have
    // checked that the position falls strictly inside the clip.
    [[nodiscard]] Clip tailAfter(FramePos position) const
    {
        const FramePos consumed = position - timelineStart;
        Clip tail = *this;
        tail.timelineStart = position;
        tail.sourceOffset = sourceOffset + consumed;
        tail.length = length - consumed;
        if (hasEnvelopes()) tail.envelopeOffset += consumed;
        if (rack) tail.effectOffset += consumed;

        if (consumed >= fadeIn - fadeInOffset) {
            tail.fadeIn = 0;
            tail.fadeInOffset = 0;
        } else {
            tail.fadeInOffset += consumed;
        }
        return tail;
    }

    [[nodiscard]] Clip headBefore(FramePos position) const
    {
        Clip head = *this;
        head.length = position - timelineStart;
        const FramePos removed = length - head.length;
        if (removed >= fadeOut - fadeOutOffset) {
            head.fadeOut = 0;
            head.fadeOutOffset = 0;
        } else {
            head.fadeOutOffset += removed;
        }
        return head;
    }
};

// New effects start at this view's current dry source interval. The caller sets
// effectOffset to zero when attaching the result; historical slices keep theirs.
inline std::shared_ptr<const ClipRack> makeClipRack(const Clip& clip, std::vector<Insert> inserts)
{
    auto result = std::make_shared<ClipRack>();
    result->inserts = std::move(inserts);
    result->source = clip.source; result->sourceOffset = clip.sourceOffset;
    result->length = clip.length; result->elastic = clip.elastic;
    if (!result->validOrigin()) throw std::invalid_argument("invalid clip effect processing origin");
    return result;
}

inline std::shared_ptr<const ClipRack> forkClipRack(const std::shared_ptr<const ClipRack>& rack)
{
    if (!rack) return {};
    auto result = std::make_shared<ClipRack>(*rack);
    result->owner = EffectOwner::clip(freshClipToken());
    for (auto& insert : result->inserts) insert.forkIdentity();
    return result;
}

} // namespace omawave::session
