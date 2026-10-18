#pragma once

#include "video/timecode.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The reference picture, through the FFmpeg process boundary (BRIEF section 2:
// never linked). ffprobe describes the file and lists every frame's timestamp;
// ffmpeg decodes from a chosen moment into a small RGBA stream that a worker
// thread reads into a bounded ring. Audio is the clock: the application asks
// for "the frame at this many seconds" and gets the newest one at or before it.
// Nothing here runs on the audio thread and nothing waits for the picture.
namespace omawave::video {

struct VideoInfo {
    bool ok = false;
    std::string error;              // why not, for a person
    std::string path;
    std::string codec;
    std::uint32_t width = 0, height = 0;
    FrameRate rate;                 // the stream's nominal rate (avg_frame_rate)
    bool variableRate = false;      // the frames' own timestamps disagree with the rate
    std::vector<double> timestamps; // every frame's presentation time, seconds, ascending
    double duration = 0.0;          // seconds
    double containerStart = 0.0;    // the file's own start time; FFmpeg seeks relative to it
    std::string timecode;           // the file's own start timecode tag, if any
    std::int64_t timecodeStart = -1;// that tag as a frame index at `rate`, -1 without

    [[nodiscard]] std::size_t frameCount() const noexcept { return timestamps.size(); }
    // The timestamp index at or before `seconds`; -1 before the first frame.
    [[nodiscard]] std::int64_t frameAt(double seconds) const noexcept;
};

// Worker thread only (it runs ffprobe twice). `cancel` aborts.
[[nodiscard]] VideoInfo probeVideo(const std::string& path, const std::atomic<bool>* cancel = nullptr);
// Whether this box can do any of this: the FFmpeg executables are present.
[[nodiscard]] std::string videoSupportReason();

struct Frame {
    double seconds = -1.0;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    [[nodiscard]] bool valid() const noexcept { return seconds >= 0.0 && !rgba.empty(); }
};

struct DecoderStats {
    std::uint64_t decoded = 0;      // frames read from ffmpeg
    std::uint64_t delivered = 0;    // frames handed to the caller
    std::uint64_t dropped = 0;      // decoded, then passed over because the clock had moved on
    std::uint64_t restarts = 0;     // ffmpeg started again for a seek or a jump
    std::uint64_t late = 0;         // asked for a frame the decoder had not reached yet
    bool running = false;           // ffmpeg is decoding right now
    bool atEnd = false;             // the stream ran out where it was last decoding
    std::string error;
};

// One decoder per reference. Thread-safe: any thread may call frameAt(); the
// worker is private. The ring holds `capacity` frames; when it is full the
// worker stops reading and ffmpeg blocks on its pipe, which is the bound.
class Decoder {
public:
    Decoder(VideoInfo info, std::uint32_t previewWidth, std::size_t capacity = 16);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    [[nodiscard]] const VideoInfo& info() const noexcept { return info_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    // Start (or restart) decoding at `seconds`: what a seek or a stop-and-look does.
    void prefetch(double seconds);
    // The newest frame at or before `seconds`. True when `out` now holds a frame
    // different from the last one delivered; false when the same frame still
    // stands or nothing is decoded yet (then `out` is untouched). A request
    // behind the ring or well ahead of it restarts the decoder there.
    // Before time zero, returns false and permits the next frame to be delivered
    // again, so a caller can clear its picture until the reference starts.
    bool frameAt(double seconds, Frame& out);
    [[nodiscard]] DecoderStats stats() const;

    // Where a restart is preferred to waiting, in seconds ahead of the newest
    // decoded frame.
    static constexpr double kJumpAhead = 1.5;

private:
    struct Slot {
        double seconds = -1.0;
        std::vector<std::uint8_t> rgba;
    };
    void work();
    bool runFrom(double seconds);       // one ffmpeg run; false to stop the worker
    void requestRestart(double seconds);

    VideoInfo info_;
    std::uint32_t width_ = 0, height_ = 0;
    std::size_t frameBytes_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> ring_;
    std::size_t head_ = 0, count_ = 0;  // ring_[head_] is the oldest
    double lastDelivered_ = -1.0;
    double newestDecoded_ = -1.0;
    double restartAt_ = -1.0;
    double runStart_ = -1.0;            // where the current or last run began
    bool restartPending_ = false;
    bool stop_ = false;
    DecoderStats stats_;
    std::thread worker_;
};

} // namespace omawave::video
