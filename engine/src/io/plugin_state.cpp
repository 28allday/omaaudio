#include "plugin_state.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace omawave::io {
namespace {
using Bytes = session::PluginState::Bytes;
constexpr std::array<std::uint8_t,8> magic{'O','M','A','P','S','T','A','T'};
constexpr auto crcTable = [] {
    std::array<std::uint32_t,256> result{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xEDB88320u : 0);
        result[i] = value;
    }
    return result;
}();
std::uint32_t crc(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t result = 0xFFFFFFFF;
    for (const auto byte : data) result = (result >> 8) ^ crcTable[(result ^ byte) & 255];
    return result ^ 0xFFFFFFFF;
}
void append(Bytes& bytes, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) { bytes.push_back(value & 255); value >>= 8; }
}
std::uint64_t integer(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}
struct Header { session::Uuid id; std::size_t uri = 0, processor = 0, controller = 0; bool hasController = false; };
bool header(std::span<const std::uint8_t> bytes, std::size_t fileSize, Header& h, std::string& error) {
    if (bytes.size() < kPluginStateHeader || fileSize < kPluginStateHeader + 4 || fileSize > kPluginStateFileLimit
        || !std::equal(magic.begin(), magic.end(), bytes.begin()) || integer(bytes,8,4) != 1 || integer(bytes,12,4) > 1) {
        error = "invalid or unsupported plugin state container"; return false;
    }
    std::copy_n(bytes.begin() + 16, 16, h.id.bytes.begin());
    h.hasController = integer(bytes,12,4) == 1;
    const auto uri = integer(bytes,32,4), processor = integer(bytes,36,8), controller = integer(bytes,44,8);
    if (h.id.isNull() || !uri || uri > session::PluginState::kUriLimit
        || processor > session::PluginState::kStreamLimit || controller > session::PluginState::kStreamLimit
        || (!h.hasController && controller != 0) || fileSize != kPluginStateHeader + uri + processor + controller + 4) {
        error = "invalid plugin state identity or stream lengths"; return false;
    }
    h.uri = uri; h.processor = processor; h.controller = controller; return true;
}
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) (void)::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void systemRequire(bool ok, const char* message) {
    if (!ok) throw std::system_error(errno, std::generic_category(), message);
}
void sync(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    systemRequire(result == 0, "sync plugin state");
}
bool unchanged(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
void readAll(int fd, std::span<std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto count = ::read(fd, bytes.data(), std::min<std::size_t>(bytes.size(), 256 * 1024));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "could not read complete plugin state"); bytes = bytes.subspan(count);
    }
}
void verifyWritten(int fd, std::span<const std::uint8_t> expected) {
    struct stat before{}, after{};
    systemRequire(::fstat(fd, &before) == 0, "inspect written plugin state");
    require(before.st_size >= 0 && std::uint64_t(before.st_size) == expected.size(), "written plugin state has changed size");
    systemRequire(::lseek(fd, 0, SEEK_SET) == 0, "rewind written plugin state");
    std::array<std::uint8_t, 65536> buffer{};
    while (!expected.empty()) {
        const auto part = std::span(buffer).first(std::min(buffer.size(), expected.size()));
        readAll(fd, part);
        require(std::equal(part.begin(), part.end(), expected.begin()), "written plugin state has changed content");
        expected = expected.subspan(part.size());
    }
    systemRequire(::fstat(fd, &after) == 0, "reinspect written plugin state");
    require(unchanged(before, after), "written plugin state changed during verification");
}
// Descriptor-relative operations keep a renamed/replaced state directory from
// redirecting a read/write through a symlink to an unrelated location.
class Directory {
public:
    Directory(const std::filesystem::path& root, bool create)
        : root_(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)),
          directory_(open(create)) {}
    int get() const { return directory_.get(); }
    void verify() const {
        struct stat pinned{}, current{};
        systemRequire(::fstat(get(), &pinned) == 0 && ::fstatat(root_.get(), "state", &current, AT_SYMLINK_NOFOLLOW) == 0,
            "inspect plugin state directory");
        require(S_ISDIR(current.st_mode) && pinned.st_dev == current.st_dev && pinned.st_ino == current.st_ino,
            "plugin state directory changed during I/O");
    }
private:
    int open(bool create) {
        systemRequire(root_.get() >= 0, "open project for plugin state");
        if (create) {
            if (::mkdirat(root_.get(), "state", 0700) != 0) systemRequire(errno == EEXIST, "create plugin state directory");
            sync(root_.get());
        }
        const int result = ::openat(root_.get(), "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        systemRequire(result >= 0, "open plugin state directory"); return result;
    }
    Fd root_, directory_;
};
Bytes readFile(const Directory& directory, const std::string& name, std::size_t remaining, bool durable = false) {
    Fd file(::openat(directory.get(), name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    systemRequire(file.get() >= 0, "open plugin state file");
    struct stat before{}, after{}, named{};
    systemRequire(::fstat(file.get(), &before) == 0, "inspect plugin state file");
    require(S_ISREG(before.st_mode) && before.st_size >= 0 && static_cast<std::uint64_t>(before.st_size) <= kPluginStateFileLimit,
        "plugin state is not a bounded regular file");
    std::array<std::uint8_t,kPluginStateHeader> first{}; readAll(file.get(), first);
    Header h; std::string error;
    if (!header(first, before.st_size, h, error)) throw std::runtime_error(error);
    require(h.processor + h.controller <= remaining, "plugin states exceed the project size limit");
    Bytes bytes(before.st_size); std::copy(first.begin(), first.end(), bytes.begin());
    readAll(file.get(), std::span(bytes).subspan(first.size()));
    if (durable) { sync(file.get()); sync(directory.get()); }
    systemRequire(::fstat(file.get(), &after) == 0 && ::fstatat(directory.get(), name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0,
        "reinspect plugin state file");
    require(unchanged(before, after) && unchanged(before, named), "plugin state changed during I/O");
    directory.verify(); return bytes;
}
}

std::vector<std::uint8_t> encodePluginState(const session::PluginState& state) {
    if (!state) throw std::invalid_argument("cannot encode an empty plugin state reference");
    Bytes bytes; bytes.reserve(kPluginStateHeader + state.uri().size() + state.bytes() + 4);
    bytes.insert(bytes.end(), magic.begin(), magic.end()); append(bytes,1,4);
    const auto controller = state.controller(); append(bytes,controller.has_value(),4);
    bytes.insert(bytes.end(), state.id().bytes.begin(), state.id().bytes.end());
    append(bytes,state.uri().size(),4); append(bytes,state.processor().size(),8); append(bytes,controller ? controller->size() : 0,8);
    bytes.insert(bytes.end(), state.uri().begin(), state.uri().end());
    bytes.insert(bytes.end(), state.processor().begin(), state.processor().end());
    if (controller) bytes.insert(bytes.end(), controller->begin(), controller->end());
    append(bytes,crc(bytes),4); return bytes;
}
std::optional<session::PluginState> decodePluginState(std::span<const std::uint8_t> bytes,
    const session::Uuid& expected, std::string_view uri, std::string& error)
{
    error.clear(); Header h;
    if (!header(bytes, bytes.size(), h, error)) return {};
    if (h.id != expected || crc(bytes.first(bytes.size() - 4)) != integer(bytes,bytes.size() - 4,4)) {
        error = "plugin state identity or checksum mismatch"; return {};
    }
    std::string stored(reinterpret_cast<const char*>(bytes.data() + kPluginStateHeader), h.uri);
    if (stored != uri) { error = "plugin state belongs to a different plugin"; return {}; }
    const auto start = bytes.begin() + kPluginStateHeader + h.uri;
    Bytes processor(start, start + h.processor);
    std::optional<Bytes> controller;
    if (h.hasController) controller = Bytes(start + h.processor, start + h.processor + h.controller);
    return session::PluginState::create(std::move(stored), std::move(processor), std::move(controller), error, expected);
}
std::optional<session::PluginState> readPluginState(const std::filesystem::path& root,
    const session::Uuid& id, std::string_view uri, std::size_t remaining, std::string& error)
{
    error.clear();
    try {
        require(!id.isNull() && session::PluginState::validUri(uri), "invalid plugin state reference");
        const Directory directory(root, false);
        return decodePluginState(readFile(directory, id.toString() + ".omas", remaining), id, uri, error);
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}
std::string writePluginState(const std::filesystem::path& root, const session::PluginState& state,
    const std::atomic<bool>* cancel, const std::function<void(float)>& progress)
{
    const auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    try {
        require(bool(state), "invalid plugin state reference"); require(!cancelled(), "plugin state save cancelled");
        const Directory directory(root, true);
        const auto name = state.id().toString() + ".omas";
        const auto bytes = encodePluginState(state);
        struct stat existing{};
        if (::fstatat(directory.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            require(readFile(directory, name, session::PluginState::kStreamLimit * 2, true) == bytes,
                "plugin state destination already contains different content");
            require(!cancelled(), "plugin state save cancelled");
            directory.verify(); return {};
        }
        systemRequire(errno == ENOENT, "inspect plugin state destination");
        const auto temporaryId = session::Uuid::generate(); require(!temporaryId.isNull(), "cannot name plugin state temporary");
        const auto temporary = ".tmp-" + temporaryId.toString();
        Fd file(::openat(directory.get(), temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        systemRequire(file.get() >= 0, "create plugin state temporary");
        struct Cleanup {
            int directory, file; const std::string& name;
            bool owns(const std::string& candidate) const {
                struct stat pinned{}, named{};
                return ::fstat(file, &pinned) == 0 && ::fstatat(directory, candidate.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISREG(named.st_mode) && pinned.st_dev == named.st_dev && pinned.st_ino == named.st_ino;
            }
            ~Cleanup() { if (owns(name)) (void)::unlinkat(directory, name.c_str(), 0); }
        } cleanup{directory.get(), file.get(), temporary};
        std::size_t done = 0;
        while (done < bytes.size()) {
            require(!cancelled(), "plugin state save cancelled");
            const auto count = ::write(file.get(), bytes.data() + done, std::min<std::size_t>(256 * 1024, bytes.size() - done));
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "could not write plugin state"); done += count;
            if (progress) progress(static_cast<float>(double(done) / bytes.size()));
        }
        require(!cancelled(), "plugin state save cancelled"); sync(file.get()); directory.verify();
        require(cleanup.owns(temporary), "plugin state temporary changed during I/O");
        verifyWritten(file.get(), bytes);
        // linkat creates the destination only when absent; a racing file survives.
        systemRequire(::linkat(directory.get(), temporary.c_str(), directory.get(), name.c_str(), 0) == 0,
            "publish immutable plugin state");
        sync(directory.get()); directory.verify();
        require(cleanup.owns(name), "plugin state destination changed during publication");
        return {};
    } catch (const std::exception& exception) { return exception.what(); }
}
}
