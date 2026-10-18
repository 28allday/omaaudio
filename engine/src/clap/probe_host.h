#pragma once

#include <clap/clap.h>
#include "host/editor.h"

#include <vector>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace omawave::clap {

// The host a plugin sees. One per instance (and one for a catalogue probe): the
// callbacks a plugin may call from any thread do nothing but record a request,
// the extensions are the small honest set (thread check, log, latency and
// parameter notifications as flags). Nothing here allocates on a plugin's
// behalf and nothing blocks.
class ProbeHost {
public:
    ProbeHost() noexcept
    {
        host_.clap_version = CLAP_VERSION_INIT;
        host_.host_data = this;
        host_.name = "OmaAudio";
        host_.vendor = "OmaAudio";
        host_.url = "https://omatools.org";
        host_.version = "0.1.0";
        host_.get_extension = &ProbeHost::getExtension;
        host_.request_restart = &ProbeHost::requestRestart;
        host_.request_process = &ProbeHost::requestProcess;
        host_.request_callback = &ProbeHost::requestCallback;
        threadCheck_.is_main_thread = &ProbeHost::isMainThread;
        threadCheck_.is_audio_thread = &ProbeHost::isAudioThread;
        log_.log = &ProbeHost::log;
        latency_.changed = &ProbeHost::latencyChanged;
        params_.rescan = &ProbeHost::paramsRescan;
        params_.clear = &ProbeHost::paramsClear;
        params_.request_flush = &ProbeHost::paramsRequestFlush;
        state_.mark_dirty = &ProbeHost::stateDirty;
        gui_.resize_hints_changed = &ProbeHost::guiHintsChanged;
        gui_.request_resize = &ProbeHost::guiRequestResize;
        gui_.request_show = &ProbeHost::guiRequestShow;
        gui_.request_hide = &ProbeHost::guiRequestHide;
        gui_.closed = &ProbeHost::guiClosed;
        timers_.register_timer = &ProbeHost::registerTimer;
        timers_.unregister_timer = &ProbeHost::unregisterTimer;
        descriptors_.register_fd = &ProbeHost::registerFd;
        descriptors_.modify_fd = &ProbeHost::modifyFd;
        descriptors_.unregister_fd = &ProbeHost::unregisterFd;
        mainThread_ = std::this_thread::get_id();
    }

    // Main thread. While a native editor is open its window host serves the
    // plug-in's timers and descriptors; without one those requests are refused.
    void attachEditor(host::EditorHost* editor, const clap_plugin_t* plugin) noexcept
    {
        if (!editor) {
            for (const auto& timer : timerIds_) if (editor_) editor_->removeEditorTimer(timer.host);
            for (const auto& fd : fdIds_) if (editor_) editor_->removeEditorDescriptor(fd.host);
            timerIds_.clear(); fdIds_.clear();
        }
        editor_ = editor; plugin_ = plugin;
        timerExt_ = plugin ? static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT)) : nullptr;
        fdExt_ = plugin ? static_cast<const clap_plugin_posix_fd_support_t*>(plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT)) : nullptr;
    }
    [[nodiscard]] bool takeEditorClosed() noexcept { return editorClosed_.exchange(false, std::memory_order_acq_rel); }
    ProbeHost(const ProbeHost&) = delete;
    ProbeHost& operator=(const ProbeHost&) = delete;

    [[nodiscard]] const clap_host_t* raw() const noexcept { return &host_; }
    [[nodiscard]] bool onMainThread() const noexcept { return std::this_thread::get_id() == mainThread_; }

    // The audio thread is the current exclusive processing owner. The host may
    // transfer a prepared processor between threads while preserving its DSP.
    void markAudioThread() noexcept { audioThread_.store(&threadToken_, std::memory_order_relaxed); }
    // A quiescent active processor may flush controls on main. That temporary
    // symbolic audio role must end before state.save, even if flush throws.
    class AudioThreadScope {
        ProbeHost& host_;
        const void* previous_;
    public:
        explicit AudioThreadScope(ProbeHost& host) noexcept : host_(host),
            previous_(host.audioThread_.exchange(&threadToken_, std::memory_order_relaxed)) {}
        AudioThreadScope(const AudioThreadScope&) = delete;
        AudioThreadScope& operator=(const AudioThreadScope&) = delete;
        ~AudioThreadScope() { host_.audioThread_.store(previous_, std::memory_order_relaxed); }
    };
    [[nodiscard]] bool restartRequested() const noexcept { return restart_.load(std::memory_order_relaxed); }
    void clearRestartRequest() noexcept { restart_.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool latencyChangedFlag() const noexcept { return latencyChanged_.load(std::memory_order_relaxed); }
    void clearLatencyChanged() noexcept { latencyChanged_.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool takeCallbackRequest() noexcept { return callback_.exchange(false, std::memory_order_acq_rel); }
    [[nodiscard]] bool takeFlushRequest() noexcept { return flush_.exchange(false, std::memory_order_acq_rel); }
    [[nodiscard]] bool takeStateDirty() noexcept { return stateDirty_.exchange(false, std::memory_order_acq_rel); }
    [[nodiscard]] bool flushRequested() const noexcept { return flush_.load(std::memory_order_acquire); }
    void requestParameterFlush() noexcept { flush_.store(true, std::memory_order_release); }
    [[nodiscard]] bool valueRescanRequested() const noexcept { return valueRescan_.load(std::memory_order_acquire); }
    [[nodiscard]] bool takeValueRescanRequest() noexcept { return valueRescan_.exchange(false, std::memory_order_acq_rel); }
    void requestValueRescan() noexcept { valueRescan_.store(true, std::memory_order_release); }
    [[nodiscard]] std::uint64_t valueRescanVersion() const noexcept { return valueRescanVersion_.load(std::memory_order_acquire); }
    [[nodiscard]] bool takeTextRescanRequest() noexcept { return textRescan_.exchange(false, std::memory_order_acq_rel); }

private:
    static ProbeHost& self(const clap_host_t* host) noexcept { return *static_cast<ProbeHost*>(host->host_data); }
    static const void* getExtension(const clap_host_t* host, const char* id) noexcept
    {
        ProbeHost& h = self(host);
        if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) return &h.threadCheck_;
        if (std::strcmp(id, CLAP_EXT_LOG) == 0) return &h.log_;
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &h.latency_;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &h.params_;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &h.state_;
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &h.gui_;
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &h.timers_;
        if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &h.descriptors_;
        return nullptr;
    }
    static void guiHintsChanged(const clap_host_t*) noexcept {}
    static bool guiRequestResize(const clap_host_t* host, std::uint32_t width, std::uint32_t height) noexcept
    {
        auto& h = self(host);
        return h.onMainThread() && h.editor_ && h.editor_->requestEditorResize(width, height);
    }
    static bool guiRequestShow(const clap_host_t* host) noexcept { return self(host).editor_ != nullptr; }
    static bool guiRequestHide(const clap_host_t*) noexcept { return false; }
    static void guiClosed(const clap_host_t* host, bool) noexcept
    {
        auto& h = self(host);
        h.editorClosed_.store(true, std::memory_order_release);
        if (h.onMainThread() && h.editor_) h.editor_->editorClosed();
    }
    static bool registerTimer(const clap_host_t* host, std::uint32_t period, clap_id* id) noexcept
    {
        auto& h = self(host);
        if (!id || !h.onMainThread() || !h.editor_ || !h.timerExt_ || !h.timerExt_->on_timer) return false;
        const clap_id clapId = ++h.nextTimer_;
        const auto* plugin = h.plugin_; const auto* ext = h.timerExt_;
        const auto hostId = h.editor_->addEditorTimer(std::max<std::uint32_t>(1, period),
            [plugin, ext, clapId] { ext->on_timer(plugin, clapId); });
        if (!hostId) return false;
        h.timerIds_.push_back({clapId, hostId});
        *id = clapId;
        return true;
    }
    static bool unregisterTimer(const clap_host_t* host, clap_id id) noexcept
    {
        auto& h = self(host);
        for (auto it = h.timerIds_.begin(); it != h.timerIds_.end(); ++it)
            if (it->clap == id) { if (h.editor_) h.editor_->removeEditorTimer(it->host); h.timerIds_.erase(it); return true; }
        return false;
    }
    static bool registerFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) noexcept
    {
        auto& h = self(host);
        if (fd < 0 || !(flags & CLAP_POSIX_FD_READ) || !h.onMainThread() || !h.editor_ || !h.fdExt_ || !h.fdExt_->on_fd) return false;
        for (const auto& known : h.fdIds_) if (known.clap == clap_id(fd)) return false;
        const auto* plugin = h.plugin_; const auto* ext = h.fdExt_;
        const auto hostId = h.editor_->addEditorDescriptor(fd, [plugin, ext, fd] { ext->on_fd(plugin, fd, CLAP_POSIX_FD_READ); });
        if (!hostId) return false;
        h.fdIds_.push_back({clap_id(fd), hostId});
        return true;
    }
    static bool modifyFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) noexcept
    {
        return unregisterFd(host, fd) && registerFd(host, fd, flags);
    }
    static bool unregisterFd(const clap_host_t* host, int fd) noexcept
    {
        auto& h = self(host);
        for (auto it = h.fdIds_.begin(); it != h.fdIds_.end(); ++it)
            if (it->clap == clap_id(fd)) { if (h.editor_) h.editor_->removeEditorDescriptor(it->host); h.fdIds_.erase(it); return true; }
        return false;
    }
    static void requestRestart(const clap_host_t* host) noexcept { self(host).restart_.store(true, std::memory_order_relaxed); }
    static void requestProcess(const clap_host_t*) noexcept {}
    static void requestCallback(const clap_host_t* host) noexcept { self(host).callback_.store(true, std::memory_order_release); }
    static bool isMainThread(const clap_host_t* host) noexcept { return std::this_thread::get_id() == self(host).mainThread_; }
    static bool isAudioThread(const clap_host_t* host) noexcept
    {
        const ProbeHost& h = self(host);
        return h.audioThread_.load(std::memory_order_relaxed) == &threadToken_;
    }
    static void log(const clap_host_t*, clap_log_severity severity, const char* message) noexcept
    {
        if (severity >= CLAP_LOG_WARNING && message != nullptr) std::fprintf(stderr, "[clap] %s\n", message);
    }
    static void latencyChanged(const clap_host_t* host) noexcept { self(host).latencyChanged_.store(true, std::memory_order_relaxed); }
    static void paramsRescan(const clap_host_t* host, clap_param_rescan_flags flags) noexcept
    {
        if (flags & CLAP_PARAM_RESCAN_TEXT) self(host).textRescan_.store(true, std::memory_order_release);
        if (flags & CLAP_PARAM_RESCAN_VALUES) {
            auto& owner = self(host);
            owner.valueRescanVersion_.store(nextValueRescanVersion_.fetch_add(1, std::memory_order_relaxed) + 1,
                                            std::memory_order_release);
            owner.requestValueRescan();
            // A preset may change values without sending process/flush events.
            // Preserve the state edit; do not synthesize automation points.
            owner.stateDirty_.store(true, std::memory_order_release);
        }
    }
    static void paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) noexcept {}
    static void paramsRequestFlush(const clap_host_t* host) noexcept { self(host).requestParameterFlush(); }
    static void stateDirty(const clap_host_t* host) noexcept { self(host).stateDirty_.store(true, std::memory_order_release); }

    clap_host_t host_{};
    clap_host_thread_check_t threadCheck_{};
    clap_host_log_t log_{};
    clap_host_latency_t latency_{};
    clap_host_params_t params_{};
    clap_host_state_t state_{};
    clap_host_gui_t gui_{};
    clap_host_timer_support_t timers_{};
    clap_host_posix_fd_support_t descriptors_{};
    host::EditorHost* editor_ = nullptr;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_timer_support_t* timerExt_ = nullptr;
    const clap_plugin_posix_fd_support_t* fdExt_ = nullptr;
    struct Registration { clap_id clap; std::uint32_t host; };
    std::vector<Registration> timerIds_, fdIds_;
    clap_id nextTimer_ = 0;
    std::atomic<bool> editorClosed_{false};
    std::thread::id mainThread_;
    // Plugins may query these callbacks from any thread. A TLS address is an
    // identity token only (never dereferenced), with a lock-free atomic handoff
    // when processing moves from warm-up to its real audio/offline thread.
    inline static thread_local const char threadToken_ = 0;
    std::atomic<const void*> audioThread_{nullptr};
    std::atomic<bool> restart_{false};
    std::atomic<bool> latencyChanged_{false};
    std::atomic<bool> callback_{false}, flush_{false}, stateDirty_{false};
    std::atomic<bool> valueRescan_{false};
    std::atomic<bool> textRescan_{false};
    // Comparable across actual instances: taking the maximum for a dual-mono
    // insert must observe a later rescan from EITHER processor.
    inline static std::atomic<std::uint64_t> nextValueRescanVersion_{0};
    std::atomic<std::uint64_t> valueRescanVersion_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<const void*>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
};

} // namespace omawave::clap
