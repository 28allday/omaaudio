#pragma once
#include "sample_edit.h"

namespace omawave::session {

enum class ChannelEdit { Gain, Normalize, Silence, FadeIn, FadeOut, Reverse, Paste, MixPaste };
struct ChannelEditSpec {
    ChannelEdit operation = ChannelEdit::Gain;
    FramePos start = 0, end = 0;
    int channel = -1; // all, or a physical source channel
    float gain = 1;
    std::vector<Clip> clipboard;
    FramePos clipboardFrames = 0;
    int clipboardChannel = -1;
    float incomingGain = 1;
    bool ripple = false;
    bool newTrack = false;
};

// Worker-only immutable edits. Mono clipboard audio duplicates into a multichannel
// destination; multichannel -> mono averages. Other mismatched layouts require an
// explicit map. For one-channel edits the destination must have a uniform layout.
[[nodiscard]] SampleEditResult editChannels(Project, TrackId, const ChannelEditSpec&,
                                            const std::atomic<bool>* cancel = nullptr);
// Rows are output channels; columns are physical inputs. Missing source inputs
// are zero. Does not apply the track's fader/pan or clip gain/fades twice.
[[nodiscard]] SampleEditResult mapTrackChannels(Project, TrackId,
    const std::vector<std::vector<float>>& matrix, const std::atomic<bool>* cancel = nullptr);
[[nodiscard]] SampleEditResult insertSilence(Project, TrackId, FramePos at, FramePos frames,
    bool allTracks, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::session
