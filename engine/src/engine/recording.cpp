#include "recording.h"

#include "io/audio_file.h"
#include "io/project_storage.h"

#include <sndfile.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omawave::engine {
Recording::Recording(CaptureClock& clock, std::string path, std::uint32_t sampleRate,
                     std::uint32_t channels, session::FramePos frameLimit)
    : clock_(clock), path_(std::move(path)), rate_(sampleRate), channels_(channels), limit_(frameLimit)
{
    if ((channels != 1 && channels != 2) || sampleRate == 0 || frameLimit < 0)
        throw std::invalid_argument("invalid recording format or length");
    writer_ = std::thread([this] { write(); });
}

Recording::~Recording()
{
    requestStop();
    if (writer_.joinable()) writer_.join();
}

void Recording::ingest(const float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept
{
    if (!ready() || inputDone() || writerFailed_.load(std::memory_order_acquire))
        return;
    if (channels != channels_) {
        overflow_.store(true, std::memory_order_relaxed);
        requestStop();
        return;
    }
    const auto limit = limit_.load(std::memory_order_acquire);
    if (limit > 0 && captured_ >= limit) { requestStop(); return; }
    if (limit > 0)
        frames = static_cast<std::uint32_t>(std::min<session::FramePos>(frames, limit - captured_));
    float peak = 0;
    for (std::uint32_t offset = 0; offset < frames;) {
        const auto count = std::min(1024U, frames - offset);
        const auto* left = planes[0] ? planes[0] + offset : nullptr;
        const auto* right = channels == 1 ? left : planes[1] ? planes[1] + offset : nullptr;
        if (!ring_.write(left, right, count)) {
            overflow_.store(true, std::memory_order_relaxed);
            requestStop();
            return;
        }
        if (clock_.monitor.load(std::memory_order_relaxed))
            (void)clock_.monitoring.write(left, right, count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (left) peak = std::max(peak, std::abs(left[i]));
            if (right) peak = std::max(peak, std::abs(right[i]));
        }
        offset += count;
        captured_ += count;
        clock_.frames.store(captured_, std::memory_order_relaxed);
        clock_.active.store(true, std::memory_order_release);
    }
    peak_.store(peak, std::memory_order_relaxed);
    if (limit > 0 && captured_ >= limit)
        requestStop();
}

void Recording::requestStop() noexcept
{
    clock_.active.store(false, std::memory_order_release);
    inputDone_.store(true, std::memory_order_release);
}

void Recording::write()
{
    result_.path = path_;
    int fd = -1;
    bool created = false;
    SNDFILE* file = nullptr;
    try {
        fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error(std::strerror(errno));
        created = true;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("recording file is locked");
        const auto directory = std::filesystem::path(path_).parent_path();
        if (const auto error = io::storage::syncPath(directory.empty() ? "." : directory, true); !error.empty())
            throw std::runtime_error(error);
        SF_INFO info{};
        info.samplerate = static_cast<int>(rate_);
        info.channels = static_cast<int>(channels_);
        info.format = SF_FORMAT_RF64 | SF_FORMAT_FLOAT;
        file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE);
        if (!file) throw std::runtime_error(sf_strerror(nullptr));
        ready_.store(true, std::memory_order_release);
        std::array<float, 2048> left{}, right{};
        std::array<float, 4096> interleaved{};
        std::uint64_t written = 0, checkpoint = 0;
        while (!inputDone() || !ring_.empty()) {
            const auto frames = ring_.read(left.data(), right.data(), left.size());
            if (frames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            for (std::size_t i = 0; i < frames; ++i) {
                interleaved[i * channels_] = left[i];
                if (channels_ == 2) interleaved[i * 2 + 1] = right[i];
            }
            if (sf_writef_float(file, interleaved.data(), static_cast<sf_count_t>(frames))
                != static_cast<sf_count_t>(frames))
                throw std::runtime_error("recording disk write failed");
            written += frames;
            if (written - checkpoint >= rate_) {
                (void)sf_command(file, SFC_UPDATE_HEADER_NOW, nullptr, 0);
                sf_write_sync(file);
                if (sf_error(file) != SF_ERR_NO_ERROR) throw std::runtime_error("recording checkpoint failed");
                if (const auto error = io::storage::syncPath(path_); !error.empty())
                    throw std::runtime_error(error);
                checkpoint = written;
            }
        }
        const int closed = sf_close(file);
        file = nullptr;
        if (closed != 0) throw std::runtime_error("recording close failed");
        if (const auto error = io::storage::syncPath(path_); !error.empty())
            throw std::runtime_error(error);
        const int closeFd = ::close(fd);
        fd = -1;
        if (closeFd != 0) throw std::runtime_error("recording file close failed");
        if (overflow_.load(std::memory_order_relaxed))
            result_.error = "recording stopped because the capture buffer overflowed; the captured portion was kept";
        if (written > 0) {
            auto loaded = io::readAudioFile(path_);
            if (!loaded.ok) throw std::runtime_error(loaded.error);
            result_.audio = std::move(loaded.audio);
        }
    } catch (const std::exception& error) {
        writerFailed_.store(true, std::memory_order_release);
        result_.error = error.what();
        if (file) (void)sf_close(file);
        if (fd >= 0) ::close(fd);
        // Keep the spool even when decoding fails; its earlier checkpoints may
        // still be recoverable. Never report an incomplete take as a clean result.
        if (created) {
            auto partial = io::readAudioFile(path_);
            if (partial.ok) result_.audio = std::move(partial.audio);
        }
    }
    clock_.active.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}

RecordingResult Recording::takeResult()
{
    if (!finished()) return {};
    if (writer_.joinable()) writer_.join();
    return std::move(result_);
}

void Recording::wait() { if (writer_.joinable()) writer_.join(); }
} // namespace omawave::engine
