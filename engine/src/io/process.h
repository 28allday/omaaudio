#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::io {

struct ProcessResult {
    bool ok = false, cancelled = false, timedOut = false;
    int exitCode = -1;
    std::string output, diagnostic, error;
};
using ProcessLine = std::function<void(std::string_view)>;
using ProcessChunk = std::function<void(std::string_view)>;
// Non-realtime only. No shell. Retains at most64 KiB per output stream and16 KiB
// per parsed stdout line. Cancelling terminates the child group and reaps its
// direct child; the system reaper owns orphaned grandchildren.
[[nodiscard]] ProcessResult runProcess(const std::vector<std::string>& args,
    const std::atomic<bool>* cancel = nullptr,
    std::chrono::milliseconds timeout = std::chrono::minutes(30), const ProcessLine& line = {},
    const ProcessChunk& stdoutChunk = {});
// stdoutChunk receives every stdout byte before line limits/retention. Throwing
// aborts and reaps the child group, allowing strict bounded helper protocols.
[[nodiscard]] std::string findExecutable(const std::string& name);

} // namespace omawave::io
