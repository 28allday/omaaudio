#include "project_storage.h"

#include "audio_file.h"

#include <algorithm>
#include <array>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace omawave::io::storage {
namespace {
namespace fs = std::filesystem;

std::string failure(std::string_view action, const fs::path& path)
{
    const int code = errno;
    return std::string(action) + " " + path.string() + ": "
         + std::error_code(code, std::generic_category()).message();
}

fs::path parent(const fs::path& path)
{
    return path.parent_path().empty() ? fs::path(".") : path.parent_path();
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) (void)::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    int close()
    {
        const int fd = fd_;
        fd_ = -1; // Linux releases the descriptor even when close reports an error.
        return ::close(fd);
    }
private:
    int fd_;
};

std::string syncDescriptor(int fd, const fs::path& path)
{
    int result;
    do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
    return result == 0 ? std::string{} : failure("could not sync", path);
}

class Temporary {
public:
    explicit Temporary(const fs::path& target)
        : name_(target.string() + ".tmp-XXXXXX"), fd_(::mkostemp(name_.data(), O_CLOEXEC)),
          owned_(fd_.get() >= 0) {}
    ~Temporary() { if (owned_) (void)::unlink(name_.c_str()); }
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;
    int fd() const { return fd_.get(); }
    const std::string& name() const { return name_; }

    std::string publish(const fs::path& target)
    {
        if (auto error = syncDescriptor(fd(), name_); !error.empty()) return error;
        if (fd_.close() != 0) return failure("could not close", name_);
        if (::rename(name_.c_str(), target.c_str()) != 0)
            return failure("could not replace", target);
        return syncPath(parent(target), true);
    }

private:
    std::string name_;
    Descriptor fd_;
    bool owned_;
};
} // namespace

BundleLock::BundleLock(const fs::path& root, bool exclusive)
{
    fd_ = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd_ < 0) { error_ = failure("could not open", root); return; }
    if (::flock(fd_, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        error_ = errno == EWOULDBLOCK ? "project is busy in another operation: " + root.string()
                                     : failure("could not lock", root);
}

BundleLock::~BundleLock() { if (fd_ >= 0) (void)::close(fd_); }

std::string syncPath(const fs::path& path, bool directory)
{
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0)));
    if (fd.get() < 0) return failure("could not open for sync", path);
    if (auto error = syncDescriptor(fd.get(), path); !error.empty()) return error;
    return fd.close() == 0 ? std::string{} : failure("could not close", path);
}

std::string makeDirectories(const fs::path& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        if (parent(path) == path) return "could not access directory " + path.string();
        if (auto error = makeDirectories(parent(path)); !error.empty()) return error;
        fs::create_directory(path, ec);
        if (ec) return "could not create " + path.string() + ": " + ec.message();
    }
    if (auto error = syncPath(path, true); !error.empty()) return error;
    return syncPath(parent(path), true);
}

std::string writeText(const fs::path& target, std::string_view body)
{
    Temporary temp(target);
    if (temp.fd() < 0) return failure("could not create temporary for", target);
    while (!body.empty()) {
        const auto size = std::min(body.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto count = ::write(temp.fd(), body.data(), size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (count == 0) errno = EIO;
            return failure("could not write", target);
        }
        body.remove_prefix(static_cast<std::size_t>(count));
    }
    return temp.publish(target);
}

std::string writeAudio(const fs::path& target, const session::AudioBuffer& audio, std::uint32_t rate,
                       const std::atomic<bool>* cancel, const std::function<void(float)>& progress)
{
    Temporary temp(target);
    if (temp.fd() < 0) return failure("could not create temporary for", target);
    if (auto error = writeFloatWav(temp.name(), audio, rate, cancel, progress); !error.empty()) return error;
    return temp.publish(target);
}

std::string copyReference(const fs::path& source, const fs::path& target,
                          const std::atomic<bool>* cancel, const std::function<void(float)>& progress)
{
    if (source.native().find('\0') != std::string::npos || target.native().find('\0') != std::string::npos)
        return "reference paths must not contain NUL characters";
    Descriptor input(::open(source.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (input.get() < 0) return failure("could not open reference", source);
    struct stat before{};
    if (::fstat(input.get(), &before) != 0) return failure("could not inspect reference", source);
    if (!S_ISREG(before.st_mode)) return "reference is not a regular file: " + source.string();
    Descriptor existing(::open(target.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (existing.get() < 0 && errno != ENOENT) return failure("could not open reference destination", target);
    struct stat stored{};
    if (existing.get() >= 0 && (::fstat(existing.get(), &stored) != 0 || !S_ISREG(stored.st_mode)
        || stored.st_size != before.st_size)) return "reference destination already exists with different content: " + target.string();
    Temporary temp(target);
    if (temp.fd() < 0) return failure("could not create temporary for", target);
    std::array<char, 256 * 1024> block{}, other{};
    const auto readBlock = [](int fd, char* data, std::size_t size) {
        std::size_t done = 0;
        while (done < size) {
            const auto count = ::read(fd, data + done, size - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            done += static_cast<std::size_t>(count);
        }
        return true;
    };
    off_t done = 0;
    while (done < before.st_size) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return "reference copy cancelled";
        const auto want = static_cast<std::size_t>(std::min<off_t>(block.size(), before.st_size - done));
        if (!readBlock(input.get(), block.data(), want)) return "could not read complete reference: " + source.string();
        if (existing.get() >= 0) {
            if (!readBlock(existing.get(), other.data(), want) || !std::equal(block.begin(), block.begin() + want, other.begin()))
                return "reference destination already exists with different content: " + target.string();
        } else {
            std::size_t written = 0;
            while (written < want) {
                const auto count = ::write(temp.fd(), block.data() + written, want - written);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return failure("could not copy reference to", target);
                written += static_cast<std::size_t>(count);
            }
        }
        done += static_cast<off_t>(want);
        if (progress) progress(static_cast<float>(double(done) / double(before.st_size)));
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return "reference copy cancelled";
    struct stat after{};
    const auto unchanged = [](const struct stat& a, const struct stat& b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
            && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
            && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    };
    if (::fstat(input.get(), &after) != 0 || !unchanged(before, after))
        return "reference changed while it was being copied: " + source.string();
    if (existing.get() >= 0) {
        if (::fstat(existing.get(), &after) != 0 || !unchanged(stored, after)
            || ::lstat(target.c_str(), &after) != 0 || !unchanged(stored, after))
            return "reference destination changed while it was being checked: " + target.string();
        return syncPath(target);
    }
    if (auto error = syncDescriptor(temp.fd(), temp.name()); !error.empty()) return error;
    // A racing file/symlink must survive, including when the initial check found no file.
    if (::link(temp.name().c_str(), target.c_str()) != 0) return failure("could not publish reference", target);
    return syncPath(parent(target), true);
}

std::string removeFile(const fs::path& path)
{
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return failure("could not remove", path);
    return syncPath(parent(path), true);
}

} // namespace omawave::io::storage
