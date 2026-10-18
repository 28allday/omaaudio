#include "reference.h"

#include "io/process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern char** environ;

namespace omawave::video {

namespace {

std::string fieldOf(const std::string& text, const std::string& key)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    return {};
}

// A child with its stdout and stderr piped back here. No shell.
class Child {
public:
    bool start(const std::vector<std::string>& args)
    {
        int out[2] = {-1, -1}, err[2] = {-1, -1};
        if (::pipe2(out, O_CLOEXEC) != 0) return false;
        if (::pipe2(err, O_CLOEXEC) != 0) { ::close(out[0]); ::close(out[1]); return false; }
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = ::posix_spawnp(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(out[1]); ::close(err[1]);
        if (rc != 0) { ::close(out[0]); ::close(err[0]); pid_ = 0; return false; }
        stdoutFd_ = out[0]; stderrFd_ = err[0];
        return true;
    }
    ~Child() { stop(); }
    void stop()
    {
        if (stdoutFd_ >= 0) { ::close(stdoutFd_); stdoutFd_ = -1; }
        if (stderrFd_ >= 0) { ::close(stderrFd_); stderrFd_ = -1; }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = 0;
        }
    }
    [[nodiscard]] int out() const noexcept { return stdoutFd_; }
    [[nodiscard]] int err() const noexcept { return stderrFd_; }
    void closeOut() { if (stdoutFd_ >= 0) { ::close(stdoutFd_); stdoutFd_ = -1; } }
    void closeErr() { if (stderrFd_ >= 0) { ::close(stderrFd_); stderrFd_ = -1; } }

private:
    pid_t pid_ = 0;
    int stdoutFd_ = -1, stderrFd_ = -1;
};

} // namespace

std::int64_t VideoInfo::frameAt(double seconds) const noexcept
{
    const auto it = std::upper_bound(timestamps.begin(), timestamps.end(), seconds + 1e-6);
    return static_cast<std::int64_t>(it - timestamps.begin()) - 1;
}

std::string videoSupportReason()
{
    if (io::findExecutable("ffmpeg").empty()) return "a video reference needs the FFmpeg executable (package ffmpeg)";
    if (io::findExecutable("ffprobe").empty()) return "a video reference needs ffprobe, which ships with FFmpeg";
    return {};
}

VideoInfo probeVideo(const std::string& path, const std::atomic<bool>* cancel)
{
    VideoInfo info;
    info.path = path;
    if (const std::string reason = videoSupportReason(); !reason.empty()) { info.error = reason; return info; }
    const std::string probe = io::findExecutable("ffprobe");

    const auto streams = io::runProcess({probe, "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,codec_type,width,height,r_frame_rate,avg_frame_rate,nb_frames,duration:stream_tags=timecode:format=duration,start_time:format_tags=timecode",
        "-of", "default=noprint_wrappers=1", path}, cancel, std::chrono::seconds(30));
    if (!streams.ok) {
        info.error = streams.cancelled ? "cancelled" : "not a readable video file: " + (streams.diagnostic.empty() ? streams.error : streams.diagnostic);
        return info;
    }
    info.codec = fieldOf(streams.output, "codec_name");
    if (info.codec.empty() || fieldOf(streams.output, "codec_type") != "video") {
        info.error = "the file has no video stream FFmpeg can read";
        return info;
    }
    info.width = static_cast<std::uint32_t>(std::atoi(fieldOf(streams.output, "width").c_str()));
    info.height = static_cast<std::uint32_t>(std::atoi(fieldOf(streams.output, "height").c_str()));
    info.rate = parseFrameRate(fieldOf(streams.output, "avg_frame_rate"));
    if (!info.rate.valid()) info.rate = parseFrameRate(fieldOf(streams.output, "r_frame_rate"));
    const std::string streamDuration = fieldOf(streams.output, "duration");
    const std::string formatDuration = [&] {
        // The format's duration is the last "duration=" line; the stream's the first.
        std::string last;
        std::istringstream lines(streams.output);
        std::string line;
        while (std::getline(lines, line)) if (line.rfind("duration=", 0) == 0) last = line.substr(9);
        return last;
    }();
    info.duration = std::atof((formatDuration.empty() ? streamDuration : formatDuration).c_str());
    info.timecode = fieldOf(streams.output, "TAG:timecode");
    if (const std::string start = fieldOf(streams.output, "start_time"); !start.empty() && start != "N/A")
        info.containerStart = std::max(0.0, std::atof(start.c_str()));
    if (info.width == 0 || info.height == 0) { info.error = "the video stream reports no picture size"; return info; }
    if (!info.rate.valid()) { info.error = "the video stream reports no frame rate"; return info; }

    // Every frame's presentation time, without decoding: the packets, sorted.
    // Taken line by line as they come; the helper keeps only the tail of a
    // long output, and an hour of picture is a hundred thousand lines.
    const auto packets = io::runProcess({probe, "-v", "error", "-select_streams", "v:0",
        "-show_entries", "packet=pts_time", "-of", "csv=p=0", path}, cancel, std::chrono::minutes(5),
        [&info](std::string_view line) {
            while (!line.empty() && (line.back() == ',' || line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
            if (line.empty() || line == "N/A") return;
            info.timestamps.push_back(std::atof(std::string(line).c_str()));
        });
    if (!packets.ok) {
        info.timestamps.clear();
        info.error = packets.cancelled ? "cancelled" : "could not read the video's frame timestamps: " + (packets.diagnostic.empty() ? packets.error : packets.diagnostic);
        return info;
    }
    std::sort(info.timestamps.begin(), info.timestamps.end());
    info.timestamps.erase(std::unique(info.timestamps.begin(), info.timestamps.end()), info.timestamps.end());
    if (info.timestamps.empty()) { info.error = "the video stream has no frames"; return info; }
    if (info.duration <= 0.0) info.duration = info.timestamps.back() + 1.0 / info.rate.fps();

    // Variable rate: an interval that is not the nominal one, beyond rounding.
    const double nominal = 1.0 / info.rate.fps();
    for (std::size_t i = 1; i < info.timestamps.size(); ++i) {
        const double gap = info.timestamps[i] - info.timestamps[i - 1];
        if (std::fabs(gap - nominal) > nominal * 0.02 + 1e-4) { info.variableRate = true; break; }
    }
    if (!info.timecode.empty()) {
        const std::int64_t start = parseTimecode(info.timecode, info.rate, info.timecode.find(';') != std::string::npos);
        info.timecodeStart = start;
    }
    info.ok = true;
    return info;
}

// ---- the decoder ----------------------------------------------------------------

Decoder::Decoder(VideoInfo info, std::uint32_t previewWidth, std::size_t capacity)
    : info_(std::move(info))
{
    width_ = std::min<std::uint32_t>(std::max<std::uint32_t>(previewWidth, 16), info_.width);
    width_ &= ~1u;
    height_ = info_.width > 0 ? static_cast<std::uint32_t>(std::llround(static_cast<double>(info_.height) * width_ / info_.width)) : 0;
    height_ = std::max<std::uint32_t>(2, height_ & ~1u);
    frameBytes_ = static_cast<std::size_t>(width_) * height_ * 4;
    ring_.resize(std::max<std::size_t>(capacity, 2));
    for (Slot& slot : ring_) slot.rgba.resize(frameBytes_);
    if (info_.ok) worker_ = std::thread([this] { work(); });
    else stats_.error = info_.error;
}

Decoder::~Decoder()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Decoder::requestRestart(double seconds)
{
    // Caller holds mutex_.
    restartAt_ = std::max(0.0, seconds);
    restartPending_ = true;
    cv_.notify_all();
}

void Decoder::prefetch(double seconds)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!info_.ok) return;
    requestRestart(seconds);
}

bool Decoder::frameAt(double seconds, Frame& out)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!info_.ok) return false;
    if (seconds < 0.0) {
        // Before the reference starts there is no picture. Allow the cached
        // first frame to be delivered again when the clock returns to it.
        lastDelivered_ = -1.0;
        return false;
    }
    // Past the last frame the last frame stands; asking beyond it would only
    // start a decode that produces nothing.
    if (!info_.timestamps.empty() && seconds > info_.timestamps.back()) seconds = info_.timestamps.back();

    // The newest frame at or before the clock.
    std::size_t best = ring_.size();
    for (std::size_t i = 0; i < count_; ++i) {
        const std::size_t at = (head_ + i) % ring_.size();
        if (ring_[at].seconds <= seconds + 1e-6) best = i;
        else break;
    }
    const bool running = stats_.running;
    if (best == ring_.size()) {
        // Nothing decoded at or before this moment. Behind what is decoded or
        // being decoded: a seek back, start again there. Decoder idle: start it
        // (unless it already ran from at or before here and hit the end). Ahead
        // by a jump of what it has reached: start again there. Otherwise it is
        // simply not there yet; give it its moment rather than restarting it
        // on every look, which would be a restart storm that never decodes.
        const double reached = newestDecoded_ >= 0.0 ? newestDecoded_ : runStart_;
        // Before the file's own first frame there is nothing earlier to find:
        // a decoder already holding, or heading for, that frame stays as it is.
        const double first = info_.timestamps.empty() ? 0.0 : info_.timestamps.front();
        const bool atFirst = seconds < first - 1e-6
            && (count_ > 0 ? ring_[head_].seconds <= first + 1e-3 : (running && runStart_ <= first + 1e-3));
        const bool behind = !atFirst
            && (count_ > 0 ? seconds < ring_[head_].seconds - 1e-6 : (running && seconds < runStart_ - 1e-6));
        const bool idle = !running && !(stats_.atEnd && seconds >= runStart_ - 1e-6);
        const bool farAhead = running && seconds > reached + kJumpAhead;
        if (!restartPending_ && (behind || idle || farAhead)) requestRestart(seconds);
        return false;
    }
    const std::size_t index = (head_ + best) % ring_.size();
    const double newest = newestDecoded_;
    if (seconds > newest + kJumpAhead && !restartPending_) {
        // The clock ran well past what is decoded: a jump. Restart there and
        // hand over the best we have meanwhile.
        if (running || stats_.atEnd) { if (!stats_.atEnd || seconds < info_.duration) requestRestart(seconds); }
    } else if (running && seconds > newest + 1e-6 && best + 1 == count_) {
        ++stats_.late;
    }
    // Everything older than the chosen frame is gone for good.
    stats_.dropped += best;
    head_ = (head_ + best) % ring_.size();
    count_ -= best;
    const Slot& slot = ring_[index];
    if (slot.seconds == lastDelivered_) { cv_.notify_all(); return false; }
    out.seconds = slot.seconds;
    out.width = width_;
    out.height = height_;
    out.rgba = slot.rgba;
    lastDelivered_ = slot.seconds;
    ++stats_.delivered;
    cv_.notify_all();      // a slot may be free now
    return true;
}

DecoderStats Decoder::stats() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void Decoder::work()
{
    for (;;) {
        double from = 0.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || restartPending_; });
            if (stop_) return;
            from = restartAt_;
            runStart_ = from;
            restartPending_ = false;
            head_ = 0; count_ = 0;
            newestDecoded_ = -1.0;
            stats_.atEnd = false;
            stats_.running = true;
            stats_.error.clear();
            ++stats_.restarts;
        }
        // Seek to the start of the frame that contains the moment (a hair
        // before it, against rounding): ffmpeg drops every frame before the
        // seek point, and the frame the clock is inside is the one wanted.
        double seekAt = from;
        if (const std::int64_t index = info_.frameAt(from); index >= 0)
            seekAt = info_.timestamps[static_cast<std::size_t>(index)] - 0.0005;
        if (!runFrom(std::max(0.0, seekAt))) return;
    }
}

bool Decoder::runFrom(double seconds)
{
    const std::string ffmpeg = io::findExecutable("ffmpeg");
    char start[64];
    std::snprintf(start, sizeof start, "%.6f", std::max(0.0, seconds - info_.containerStart));
    const std::string scale = "scale=" + std::to_string(width_) + ":" + std::to_string(height_) + ":flags=bilinear,showinfo";
    Child child;
    // -copyts keeps the file's own timestamps after the seek (without it ffmpeg
    // re-bases them to zero), so showinfo reports where each frame really is.
    // The seek is the other way about: ffmpeg adds the file's start time to
    // -ss, for the seek and for the frames it then drops, so a file that starts
    // late is asked for the moment less that start.
    const bool started = !ffmpeg.empty() && child.start({ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info",
        "-ss", start, "-copyts", "-i", info_.path, "-map", "0:v:0", "-an", "-sn", "-dn",
        "-fps_mode", "passthrough", "-vf", scale, "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
    if (!started) {
        const std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = false;
        stats_.error = ffmpeg.empty() ? "FFmpeg is not installed" : "FFmpeg could not be started";
        return true;
    }

    std::vector<std::uint8_t> pending(frameBytes_);
    std::size_t filled = 0;
    std::string errText;
    std::vector<double> times;           // pts_time in the order showinfo reports them
    std::size_t timesUsed = 0;
    bool outOpen = true, errOpen = true;
    std::string tail;                    // unparsed stderr remainder

    // A frame is labelled by showinfo's line for it, which ffmpeg writes before
    // the frame's bytes; the two pipes are read as they come, so a complete
    // frame waits for its line rather than taking a guess. Only a build whose
    // showinfo never speaks (stderr closed, no lines) falls back to the probe's
    // list counted from the seek point.
    while (outOpen || errOpen) {
        bool ringFull = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return false;
            if (restartPending_) return true;
            ringFull = count_ >= ring_.size();
        }
        const bool frameReady = filled == frameBytes_;
        if (frameReady && (timesUsed < times.size() || !errOpen)) {
            double at;
            if (timesUsed < times.size()) at = times[timesUsed++];
            else {
                const std::int64_t first = static_cast<std::int64_t>(std::lower_bound(info_.timestamps.begin(), info_.timestamps.end(), seconds - 1e-6) - info_.timestamps.begin());
                const std::size_t index = static_cast<std::size_t>(first) + timesUsed++;
                at = index < info_.timestamps.size() ? info_.timestamps[index] : seconds + static_cast<double>(timesUsed) / info_.rate.fps();
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return false;
            if (restartPending_) return true;
            if (count_ < ring_.size()) {
                Slot& slot = ring_[(head_ + count_) % ring_.size()];
                slot.seconds = at;
                std::memcpy(slot.rgba.data(), pending.data(), frameBytes_);
                ++count_;
                newestDecoded_ = at;
                ++stats_.decoded;
                filled = 0;
                continue;
            }
            ringFull = true;
        }
        pollfd fds[2];
        int n = 0;
        int outIndex = -1, errIndex = -1;
        // Stop reading frames while the ring is full or a frame awaits its
        // label: ffmpeg then blocks on its pipe, which is the bound.
        if (outOpen && !ringFull && !frameReady) { fds[n] = {child.out(), POLLIN, 0}; outIndex = n++; }
        if (errOpen) { fds[n] = {child.err(), POLLIN, 0}; errIndex = n++; }
        if (n == 0) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stop_ || restartPending_ || count_ < ring_.size(); });
            continue;
        }
        const int ready = ::poll(fds, static_cast<nfds_t>(n), 50);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) {
            if (ringFull) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stop_ || restartPending_ || count_ < ring_.size(); });
            }
            continue;
        }
        if (errIndex >= 0 && (fds[errIndex].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[4096];
            const ssize_t got = ::read(child.err(), buffer, sizeof buffer);
            if (got <= 0) { errOpen = false; child.closeErr(); }
            else {
                tail.append(buffer, static_cast<std::size_t>(got));
                std::size_t nl;
                while ((nl = tail.find('\n')) != std::string::npos) {
                    const std::string line = tail.substr(0, nl);
                    tail.erase(0, nl + 1);
                    const auto at = line.find("pts_time:");
                    if (at != std::string::npos && line.find("showinfo") != std::string::npos) {
                        times.push_back(std::atof(line.c_str() + at + 9));
                    } else if (line.find("Error") != std::string::npos || line.find("Invalid") != std::string::npos) {
                        if (errText.size() < 512) errText += line + "\n";
                    }
                }
            }
        }
        if (outIndex >= 0 && (fds[outIndex].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t got = ::read(child.out(), pending.data() + filled, frameBytes_ - filled);
            if (got <= 0) { outOpen = false; child.closeOut(); }
            else filled += static_cast<std::size_t>(got);
        }
    }
    if (std::getenv("OMAAUDIO_VIDEO_DEBUG") != nullptr)
        std::fprintf(stderr, "[video] run from %.3f ended: %llu decoded, %zu times, err: %s\n", seconds,
                     static_cast<unsigned long long>(stats_.decoded), times.size(), errText.c_str());
    const std::lock_guard<std::mutex> lock(mutex_);
    stats_.running = false;
    stats_.atEnd = true;
    if (stats_.decoded == 0 && !errText.empty()) stats_.error = "FFmpeg could not decode the picture: " + errText;
    return true;
}

} // namespace omawave::video
