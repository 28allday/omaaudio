#include "server.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

namespace omawave::ipc {

Server::Server()
{
    wakeFd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

Server::~Server()
{
    closeClient();
    if (listener_ >= 0) {
        ::close(listener_);
        listener_ = -1;
    }
    if (wakeFd_ >= 0) {
        ::close(wakeFd_);
        wakeFd_ = -1;
    }
    if (tickFd_ >= 0) {
        ::close(tickFd_);
        tickFd_ = -1;
    }
    removeOwnedSocket();
}

void Server::removeOwnedSocket() noexcept
{
    struct stat current{};
    if (!path_.empty() && ::lstat(path_.c_str(), &current) == 0
        && S_ISSOCK(current.st_mode)
        && static_cast<std::uint64_t>(current.st_dev) == pathDevice_
        && static_cast<std::uint64_t>(current.st_ino) == pathInode_)
        ::unlink(path_.c_str());
    path_.clear();
}

bool Server::listen(const std::string& path, std::string& error)
{
    error.clear();
    if (listener_ >= 0) {
        error = "server is already listening";
        return false;
    }
    if (path.empty() || path.find('\0') != std::string::npos) {
        error = "socket path must be nonempty and contain no NUL bytes";
        return false;
    }
    if (wakeFd_ < 0) {
        error = "eventfd failed";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long";
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    // The socket usually lives under XDG_RUNTIME_DIR, which is tmpfs and so is empty
    // again after every reboot: the directory holding the socket has to be made, not
    // assumed, or bind fails with ENOENT and the engine dies on the first run of the
    // session. 0700 because a control socket is nobody else's business.
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        if (::mkdir(dir.c_str(), 0700) < 0 && errno != EEXIST) {
            error = std::string("mkdir ") + dir + ": " + std::strerror(errno);
            return false;
        }
    }

    // bind() refuses existing files, links and sockets atomically. Do not unlink
    // blindly: it may be another live engine's socket or an unrelated original.
    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int code = errno;
        error = std::string("bind: ") + std::strerror(code);
        if (code == EADDRINUSE)
            error += "; existing socket paths require explicit cleanup if stale";
        ::close(listener_);
        listener_ = -1;
        return false;
    }
    struct stat bound{};
    if (::lstat(path.c_str(), &bound) < 0 || !S_ISSOCK(bound.st_mode)) {
        error = "bound socket path disappeared or was replaced";
        ::close(listener_);
        listener_ = -1;
        return false;
    }
    path_ = path;
    pathDevice_ = static_cast<std::uint64_t>(bound.st_dev);
    pathInode_ = static_cast<std::uint64_t>(bound.st_ino);
    if (::listen(listener_, 1) < 0) {
        error = std::string("listen: ") + std::strerror(errno);
        ::close(listener_);
        listener_ = -1;
        removeOwnedSocket();
        return false;
    }

    return true;
}

void Server::closeClient() noexcept
{
    if (client_ >= 0) {
        ::close(client_);
        client_ = -1;
        ++clientGeneration_;
    }
    stream_ = MessageStream{};
    output_.clear();
    outputOffset_ = 0;
}

void Server::stop() noexcept
{
    stopped_ = true;
    wake();
}

void Server::wake() noexcept
{
    if (wakeFd_ < 0)
        return;
    const std::uint64_t one = 1;
    const ssize_t n = ::write(wakeFd_, &one, sizeof(one));
    (void)n;   // a full counter means the loop is already about to wake
}

void Server::flushOutput() noexcept
{
    if (client_ < 0)
        return;

    while (outputOffset_ < output_.size()) {
        const ssize_t n = ::send(client_, output_.data() + outputOffset_,
                                 output_.size() - outputOffset_,
                                 MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return; // resume at POLLOUT, preserving the unsent suffix
            if (n < 0 && errno == EINTR)
                continue;
            // The UI went away mid-write. Dropping the connection is the honest
            // outcome; the engine keeps running and waits for it to come back.
            closeClient();
            return;
        }
        outputOffset_ += static_cast<std::size_t>(n);
    }
    output_.clear();
    outputOffset_ = 0;
}

void Server::send(const Message& message)
{
    if (client_ < 0)
        return;
    const std::string bytes = message.encodeForWire();
    const std::size_t pending = output_.size() - outputOffset_;
    if (bytes.size() > kMaxQueuedBytes - pending) {
        closeClient();
        return;
    }
    if (outputOffset_ != 0) {
        output_.erase(0, outputOffset_);
        outputOffset_ = 0;
    }
    output_ += bytes;
    flushOutput();
}

void Server::run(int timeoutSeconds)
{
    int deadlineFd = -1;
    if (timeoutSeconds > 0) {
        deadlineFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (deadlineFd >= 0) {
            itimerspec its{};
            its.it_value.tv_sec = timeoutSeconds;
            ::timerfd_settime(deadlineFd, 0, &its, nullptr);
        }
    }

    if (onTick_ && tickIntervalMs_ > 0 && tickFd_ < 0) {
        tickFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tickFd_ >= 0) {
            itimerspec its{};
            its.it_value.tv_sec = tickIntervalMs_ / 1000;
            its.it_value.tv_nsec = (tickIntervalMs_ % 1000) * 1'000'000L;
            its.it_interval = its.it_value;
            ::timerfd_settime(tickFd_, 0, &its, nullptr);
        }
    }

    std::array<std::uint8_t, 65536> buffer{};

    while (!stopped_) {
        std::array<pollfd, 5> pfds{};
        int n = 0;

        pfds[n++] = pollfd{.fd = listener_, .events = POLLIN, .revents = 0};
        pfds[n++] = pollfd{.fd = wakeFd_, .events = POLLIN, .revents = 0};

        const int clientSlot = client_ >= 0 ? n : -1;
        const auto polledGeneration = clientGeneration_;
        if (client_ >= 0)
            pfds[n++] = pollfd{.fd = client_,
                .events = static_cast<short>(POLLIN | (output_.empty() ? 0 : POLLOUT)),
                .revents = 0};

        const int tickSlot = tickFd_ >= 0 ? n : -1;
        if (tickFd_ >= 0)
            pfds[n++] = pollfd{.fd = tickFd_, .events = POLLIN, .revents = 0};

        const int deadlineSlot = deadlineFd >= 0 ? n : -1;
        if (deadlineFd >= 0)
            pfds[n++] = pollfd{.fd = deadlineFd, .events = POLLIN, .revents = 0};

        if (::poll(pfds.data(), static_cast<nfds_t>(n), -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (deadlineSlot >= 0 && (pfds[deadlineSlot].revents & POLLIN) != 0)
            break;
        if (stopped_)
            break;

        if (tickSlot >= 0 && (pfds[tickSlot].revents & POLLIN) != 0) {
            std::uint64_t expirations = 0;
            const ssize_t got = ::read(tickFd_, &expirations, sizeof(expirations));
            (void)got;
            if (onTick_)
                onTick_();
            if (stopped_)
                break;
        }

        if ((pfds[1].revents & POLLIN) != 0) {
            std::uint64_t drained = 0;
            const ssize_t got = ::read(wakeFd_, &drained, sizeof(drained));
            (void)got;
            if (onWake_)
                onWake_();
            if (stopped_)
                break;
        }

        if ((pfds[0].revents & POLLIN) != 0) {
            const int incoming = ::accept4(listener_, nullptr, nullptr,
                                           SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (incoming >= 0) {
                if (client_ >= 0) {
                    // Already serving a UI. Refuse rather than multiplex.
                    ::close(incoming);
                } else {
                    client_ = incoming;
                    ++clientGeneration_;
                    stream_ = MessageStream{};
                    if (onConnect_)
                        onConnect_();
                }
            }
        }

        // Callbacks and accept may have replaced a client, even reusing its fd.
        // Readiness belongs to the generation polled, not to that integer alone.
        if (stopped_)
            break;
        if (clientSlot < 0 || client_ < 0 || clientGeneration_ != polledGeneration)
            continue;
        const short events = pfds[clientSlot].revents;
        if ((events & (POLLERR | POLLNVAL)) != 0) {
            closeClient();
            continue;
        }
        if ((events & POLLOUT) != 0)
            flushOutput();
        if (client_ < 0)
            continue;
        if ((events & (POLLIN | POLLHUP)) != 0) {
            const ssize_t got = ::read(client_, buffer.data(), buffer.size());
            if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (got <= 0) {
                closeClient();
                continue;
            }

            stream_.append(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::size_t>(got));
            while (const std::optional<Message> m = stream_.next()) {
                if (onMessage_)
                    onMessage_(*m);
                if (stopped_ || clientGeneration_ != polledGeneration)
                    break;
            }

            if (stream_.failed()) {
                // Only an oversized message reaches here. A merely malformed message
                // is skipped by next() without losing stream sync, because the
                // delimiter says exactly where it ended.
                std::fprintf(stderr, "[ipc] %s -- dropping client\n",
                             stream_.error().c_str());
                closeClient();
            }
        }
    }

    if (deadlineFd >= 0)
        ::close(deadlineFd);
}

} // namespace omawave::ipc
