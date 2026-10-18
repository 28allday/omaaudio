#pragma once

#include "session/audio_buffer.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace omawave::io::storage {

// Linux bundle I/O, on the worker only. A lock is held across the whole operation,
// including audio, so cooperating readers never observe half of a metadata pair.
class BundleLock {
public:
    BundleLock(const std::filesystem::path& root, bool exclusive);
    ~BundleLock();
    BundleLock(const BundleLock&) = delete;
    BundleLock& operator=(const BundleLock&) = delete;
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    int fd_ = -1;
    std::string error_;
};

[[nodiscard]] std::string syncPath(const std::filesystem::path& path, bool directory = false);
[[nodiscard]] std::string makeDirectories(const std::filesystem::path& path);
[[nodiscard]] std::string writeText(const std::filesystem::path& target, std::string_view body);
[[nodiscard]] std::string writeAudio(const std::filesystem::path& target,
                                    const session::AudioBuffer& audio, std::uint32_t rate,
                                    const std::atomic<bool>* cancel = nullptr,
                                    const std::function<void(float)>& progress = {});
// Copy regular reference media, checking that the source did not change.
// Never replace an existing target; an identical regular file may be reused.
[[nodiscard]] std::string copyReference(const std::filesystem::path& source,
    const std::filesystem::path& target, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(float)>& progress = {});
[[nodiscard]] std::string removeFile(const std::filesystem::path& path);

} // namespace omawave::io::storage
