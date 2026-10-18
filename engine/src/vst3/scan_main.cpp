#include "scan.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define OMA_VST3_SCAN_SANITIZED 1
#endif
#endif

int main(int argc, char** argv)
{
    if (argc != 3 || std::strcmp(argv[1], "--scan") != 0) return 2;
    // Keep the protocol separate from arbitrary plugin stdout. The duplicate
    // closes in any exec'd descendant. Parent drains bounded diagnostics.
    const int output = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (output < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 3;
    const rlimit core{0, 0}; (void)::setrlimit(RLIMIT_CORE, &core);
    // The helper is failure isolation, not a security sandbox. Limit runaway
    // address-space allocation too. ASan/TSan require enormous shadow mappings;
    // only instrumented verification builds omit this address-space limit.
#if !defined(OMA_VST3_SCAN_SANITIZED) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    rlimit memory{};
    if (::getrlimit(RLIMIT_AS, &memory) != 0) { ::close(output); return 5; }
    constexpr rlim_t limit = rlim_t{4} * 1024 * 1024 * 1024;
    memory.rlim_cur = std::min(memory.rlim_cur, limit);
    memory.rlim_max = std::min(memory.rlim_max, limit);
    if (::setrlimit(RLIMIT_AS, &memory) != 0) { ::close(output); return 5; }
#endif
    std::string response;
    try { response = omawave::vst3::encodeScan(omawave::vst3::inspectBundle(argv[2])); }
    catch (const std::exception& error) {
        omawave::vst3::ScanResult failed; failed.error = error.what();
        response = omawave::vst3::encodeScan(failed);
    }
    std::size_t done = 0;
    while (done < response.size()) {
        const auto written = ::write(output, response.data() + done, response.size() - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ::close(output); return 4; }
        done += static_cast<std::size_t>(written);
    }
    ::close(output);
    // Normal return also exercises module/static teardown. A crash or hang here
    // must invalidate the parent's result despite a complete protocol response.
    return 0;
}
