#pragma once

#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::ipc {

// Unix socket server for a single UI client.
//
// One client is all a plugin needs, and refusing extras is simpler to reason about
// than multiplexing. A second connection is accepted and immediately closed rather
// than being left hanging.
//
// All sending happens on the loop thread. Workers call wake() and let the loop do the
// writing, so there is exactly one thread touching the socket.
class Server {
public:
    using MessageHandler = std::function<void(const Message&)>;
    using WakeHandler = std::function<void()>;
    using ConnectHandler = std::function<void()>;
    using TickHandler = std::function<void()>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] bool listen(const std::string& path, std::string& error);

    void onMessage(MessageHandler h) { onMessage_ = std::move(h); }
    void onWake(WakeHandler h) { onWake_ = std::move(h); }
    void onConnect(ConnectHandler h) { onConnect_ = std::move(h); }

    // Periodic callback on the loop thread. Used to push the playhead at the UI's
    // frame rate; spike C showed 60Hz costs the UI nothing measurable.
    void onTick(TickHandler h, int intervalMs)
    {
        onTick_ = std::move(h);
        tickIntervalMs_ = intervalMs;
    }

    // Blocks until stop() is called or the deadline elapses. `timeoutSeconds` of 0
    // means run until stopped.
    void run(int timeoutSeconds = 0);

    // Thread safe; wakes a blocked poll as well as setting the stop flag.
    void stop() noexcept;

    // Safe to call from any thread: nudges the loop so it can do work on behalf of a
    // worker without the worker touching the socket.
    void wake() noexcept;

    // Loop thread only.
    void send(const Message& message);

    [[nodiscard]] bool connected() const noexcept { return client_ >= 0; }

private:
    void closeClient() noexcept;
    void flushOutput() noexcept;
    void removeOwnedSocket() noexcept;

    int listener_ = -1;
    int client_ = -1;
    int wakeFd_ = -1;
    int tickFd_ = -1;
    int tickIntervalMs_ = 0;
    std::string path_;
    std::uint64_t pathDevice_ = 0;
    std::uint64_t pathInode_ = 0;
    std::atomic<bool> stopped_{false};
    std::uint64_t clientGeneration_ = 0;

    // A stalled reader cannot retain unbounded status/peak messages or block the
    // loop. Disconnect once the unsent queue exceeds this limit.
    static constexpr std::size_t kMaxQueuedBytes = 4u * 1024u * 1024u;
    std::string output_;
    std::size_t outputOffset_ = 0;

    MessageStream stream_;

    MessageHandler onMessage_;
    WakeHandler onWake_;
    ConnectHandler onConnect_;
    TickHandler onTick_;
};

} // namespace omawave::ipc
