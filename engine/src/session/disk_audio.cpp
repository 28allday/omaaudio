#include "disk_audio.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omawave::session {
namespace {
[[noreturn]] void fileError(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}
// Out of space (or over a file-size or quota limit): say so in plain words, with
// where the working audio goes and roughly how much it needs.
[[noreturn]] void storageError(const char* operation, const std::filesystem::path& where, std::size_t bytes)
{
    const int error = errno;
    if (error == ENOSPC || error == EDQUOT || error == EFBIG) {
        const auto megabytes = (bytes + (1u << 20) - 1) >> 20;
        throw std::runtime_error("Not enough disk space for OmaAudio's working audio in " + where.string() + " (needs about "
            + std::to_string(megabytes) + " MB). Free some space on that drive and try again.");
    }
    errno = error;
    fileError(operation);
}
std::filesystem::path scratchDirectory()
{
    std::filesystem::path base;
    if (const char* cachePath = std::getenv("XDG_CACHE_HOME"); cachePath && *cachePath)
        base = cachePath;
    else if (const char* userDirectory = std::getenv("HOME"); userDirectory && *userDirectory)
        base = std::filesystem::path(userDirectory) / ".cache";
    else throw std::runtime_error("no user cache directory is available for audio storage");
    const auto path = base / "omaaudio" / "audio-scratch";
    std::filesystem::create_directories(path);
    if (::chmod(path.c_str(), 0700) != 0) fileError("protect audio scratch directory");
    return path;
}
}

std::shared_ptr<DiskAudio> DiskAudio::create(std::uint32_t channels, std::size_t frames,
                                             const std::string& directory)
{
    constexpr auto maximum = std::size_t(std::numeric_limits<off_t>::max());
    if (!channels || !frames || frames > maximum / sizeof(float) / channels)
        throw std::runtime_error("invalid disk audio geometry");
    const auto bytes = frames * channels * sizeof(float);
    auto out = std::shared_ptr<DiskAudio>(new DiskAudio(channels, frames, bytes));
    const auto parent = directory.empty() ? scratchDirectory() : std::filesystem::path(directory);
    const auto name = (parent / "source-XXXXXX").string();
    std::vector<char> pattern(name.begin(), name.end()); pattern.push_back('\0');
    out->fd_ = ::mkostemp(pattern.data(), O_CLOEXEC);
    if (out->fd_ < 0) fileError("create private audio storage");
    if (::unlink(pattern.data()) != 0) fileError("unlink private audio storage");
    if (::ftruncate(out->fd_, off_t(bytes)) != 0) storageError("size private audio storage", parent, bytes);
    out->directory_ = parent.string();
    return out;
}

DiskAudio::~DiskAudio()
{
    if (mapping_) ::munmap(const_cast<float*>(mapping_), bytes_);
    if (fd_ >= 0) ::close(fd_);
}

void DiskAudio::writePlane(std::uint32_t channel, std::size_t frame, std::span<const float> samples)
{
    if (sealed() || channel >= channels_ || frame > frames_ || samples.size() > frames_ - frame)
        throw std::runtime_error("invalid write to disk audio");
    auto offset = off_t((std::size_t(channel) * strideFrames_ + frame) * sizeof(float));
    const auto* data = reinterpret_cast<const char*>(samples.data());
    auto remaining = samples.size_bytes();
    while (remaining) {
        const auto count = ::pwrite(fd_, data, remaining, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; storageError("write audio storage", directory_, bytes_); }
        const auto written = std::size_t(count);
        remaining -= written; data += written; offset += count;
    }
}

void DiskAudio::seal(std::size_t validFrames)
{
    if (sealed()) return;
    if (validFrames > frames_) throw std::runtime_error("disk audio prefix exceeds stored geometry");
    if (validFrames) frames_ = validFrames;
    void* mapped = ::mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) fileError("map immutable audio storage");
    mapping_ = static_cast<const float*>(mapped);
}

std::span<const float> DiskAudio::plane(std::uint32_t channel) const noexcept
{
    assert(sealed() && channel < channels_);
    return {mapping_ + std::size_t(channel) * strideFrames_, frames_};
}

bool DiskAudio::readPlane(std::uint32_t channel, std::size_t frame, std::span<float> samples) const noexcept
{
    if (channel >= channels_ || frame > frames_ || samples.size() > frames_ - frame) return false;
    auto offset = off_t((std::size_t(channel) * strideFrames_ + frame) * sizeof(float));
    auto* data = reinterpret_cast<char*>(samples.data()); auto remaining = samples.size_bytes();
    while (remaining) {
        const auto count = ::pread(fd_, data, remaining, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        const auto read = std::size_t(count);
        remaining -= read; data += read; offset += count;
    }
    return true;
}

void DiskAudio::releaseMappedPages() const noexcept
{
    if (mapping_) (void)::madvise(const_cast<float*>(mapping_), bytes_, MADV_DONTNEED);
}

} // namespace omawave::session
