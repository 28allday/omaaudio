#include "scan.h"
#include "io/process.h"
#include <pluginterfaces/vst/ivstcomponent.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>

namespace omawave::vst3 {
namespace fs = std::filesystem;
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("invalid or incomplete VST3 scanner response"); }
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> parts;
    while (!line.empty()) {
        require(parts.size() < 16);
        const auto space = line.find(' '); parts.push_back(line.substr(0, space));
        require(!parts.back().empty());
        if (space == line.npos) break;
        line.remove_prefix(space + 1); require(!line.empty());
    }
    require(!parts.empty()); return parts;
}
template<class T> T number(std::string_view text, T maximum) {
    T value{}; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= T{} && value <= maximum);
    return value;
}
std::string unhex(std::string_view text, std::size_t maximum = 1024) {
    if (text == "-") return {};
    require(text.size() % 2 == 0 && text.size() / 2 <= maximum);
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        require(c >= 'A' && c <= 'F'); return c - 'A' + 10;
    };
    std::string result; result.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto c = static_cast<char>(digit(text[i]) * 16 + digit(text[i + 1]));
        require(c != 0); result += c;
    }
    return result;
}
class Reader {
public:
    ScanResult result;
    void bytes(std::string_view bytes) {
        require(bytes.size() <= kScanBytes - used_); used_ += bytes.size();
        for (char c : bytes) {
            if (c == '\n') { line(pending_); pending_.clear(); }
            else { require(pending_.size() < 16384); pending_ += c; }
        }
    }
    void finish(const std::string& expectedPath) {
        require(ended_ && pending_.empty());
        if (result.error.empty()) { require(result.path == expectedPath); result.ok = true; }
    }
private:
    void line(std::string_view text) {
        require(!ended_);
        if (!header_) { require(text == "OMAAUDIO-VST3-SCAN-2"); header_ = true; return; }
        const auto f = fields(text);
        if (f[0] == "ERR") {
            require(f.size() == 2 && result.path.empty() && result.error.empty() && result.plugins.empty());
            result.error = unhex(f[1]); require(!result.error.empty()); return;
        }
        if (f[0] == "M") {
            require(f.size() == 2 && result.path.empty() && result.error.empty() && result.plugins.empty());
            result.path = unhex(f[1], 4096); require(!result.path.empty()); return;
        }
        if (f[0] == "END") {
            require(f.size() == 2 && !active_ && (!result.path.empty() || !result.error.empty()));
            require(number<std::size_t>(f[1], kScanClasses) == result.plugins.size()); ended_ = true; return;
        }
        require(!result.path.empty() && result.error.empty());
        if (f[0] == "P") {
            require(!active_ && f.size() == 13 && result.plugins.size() < kScanClasses);
            Steinberg::FUID id;
            require(parseClassId(f[1], id) && classIdString(id.toTUID()) == f[1] && ids_.insert(std::string(f[1])).second);
            ScanPlugin p; p.info.id = f[1]; p.info.name = unhex(f[2]); p.info.category = unhex(f[3]);
            require(p.info.category == "Audio Module Class");
            p.info.subcategories = unhex(f[4]); p.info.vendor = unhex(f[5]); p.info.version = unhex(f[6]);
            p.info.sdkVersion = unhex(f[7]); p.error = unhex(f[8]); p.sample32 = number<unsigned>(f[9], 1);
            buses_ = number<std::size_t>(f[10], 256); parameters_ = number<std::size_t>(f[11], kScanParameters);
            layouts_ = number<std::size_t>(f[12], 2);
            require(p.error.empty() || (buses_ == 0 && parameters_ == 0 && layouts_ == 0));
            require(p.sample32 || layouts_ == 0);
            result.plugins.push_back(std::move(p)); active_ = true; busIndices_ = {}; parameterIds_.clear(); return;
        }
        require(active_); auto& p = result.plugins.back();
        if (f[0] == "L") {
            require(f.size() == 3 && p.layouts.size() < layouts_ && p.buses.empty() && p.parameters.empty());
            ScanLayout layout{number<std::uint32_t>(f[1], 2), number<std::uint32_t>(f[2], 2)};
            require(layout.channels > 0 && std::none_of(p.layouts.begin(), p.layouts.end(),
                [&](const auto& other) { return other.channels == layout.channels; }));
            p.layouts.push_back(layout); return;
        }
        require(p.layouts.size() == layouts_);
        if (f[0] == "B") {
            require(f.size() == 8 && p.buses.size() < buses_ && p.parameters.empty());
            ScanBus b; b.input = number<unsigned>(f[1], 1); b.event = number<unsigned>(f[2], 1);
            b.index = number<unsigned>(f[3], 63); b.channels = number<unsigned>(f[4], 256);
            b.type = number<unsigned>(f[5], 1); b.flags = number<uint32_t>(f[6], UINT32_MAX); b.name = unhex(f[7]);
            require(b.index == busIndices_[b.input + 2 * b.event]++); p.buses.push_back(std::move(b)); return;
        }
        if (f[0] == "C") {
            require(f.size() == 8 && p.buses.size() == buses_ && p.parameters.size() < parameters_);
            ScanParameter v; v.id = number<uint32_t>(f[1], INT32_MAX);
            require(parameterIds_.insert(v.id).second); v.steps = number<uint32_t>(f[2], INT32_MAX);
            v.flags = number<uint32_t>(f[3], UINT32_MAX); v.defaultValue = number<double>(f[4], 1);
            require(std::isfinite(v.defaultValue)); v.name = unhex(f[5]); v.shortName = unhex(f[6]); v.units = unhex(f[7]);
            p.parameters.push_back(std::move(v)); return;
        }
        require(f[0] == "E" && f.size() == 1 && p.buses.size() == buses_ && p.parameters.size() == parameters_);
        if (!p.layouts.empty()) {
            unsigned mains[2]{}; bool key = false;
            for (const auto& bus : p.buses) {
                if (bus.event) continue;
                require(bus.channels <= 64);
                const bool cv = bus.flags & Steinberg::Vst::BusInfo::kIsControlVoltage;
                if (bus.type == Steinberg::Vst::kMain) { require(!cv); ++mains[bus.input]; }
                else if (bus.input && !cv) key = true;
            }
            require(mains[0] == 1 && mains[1] == 1);
            for (const auto& layout : p.layouts) require(key || layout.sidechainChannels == 0);
        }
        active_ = false;
    }
    std::string pending_;
    std::size_t used_ = 0, buses_ = 0, parameters_ = 0, layouts_ = 0;
    std::array<unsigned, 4> busIndices_{};
    std::set<std::string> ids_;
    std::set<uint32_t> parameterIds_;
    bool header_ = false, active_ = false, ended_ = false;
};
bool cancelled(const std::atomic<bool>* stop) { return stop && stop->load(std::memory_order_relaxed); }
}

ScanResult scanBundle(const std::string& helper, const std::string& bundle,
    const std::atomic<bool>* cancel, std::chrono::milliseconds timeout)
{
    ScanResult result;
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    if (helper.empty() || !fs::path(helper).is_absolute() || helper.find('\0') != helper.npos
        || bundle.empty() || bundle.find('\0') != bundle.npos || bundle.size() > 4096) {
        result.error = "invalid VST3 scanner helper or bundle path"; return result;
    }
    std::error_code ec; const auto canonical = fs::canonical(bundle, ec);
    if (ec || canonical.extension() != ".vst3" || !fs::is_directory(canonical, ec)) {
        result.error = "VST3 bundle is not a readable .vst3 directory"; return result;
    }
    Reader reader;
    const auto process = io::runProcess({helper, "--scan", canonical.string()}, cancel, timeout, {},
        [&](std::string_view bytes) { reader.bytes(bytes); });
    result.cancelled = process.cancelled; result.timedOut = process.timedOut; result.diagnostic = process.diagnostic;
    if (!process.ok) { result.error = process.error; return result; }
    try {
        reader.finish(canonical.string());
        result = std::move(reader.result); result.diagnostic = process.diagnostic;
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

std::vector<std::string> searchPath(const std::string& applicationFolder)
{
    std::vector<std::string> paths;
    if (const auto* home = std::getenv("HOME"); home && *home) paths.push_back((fs::path(home) / ".vst3").string());
    for (const auto* path : {"/usr/lib64/vst3", "/usr/lib/vst3", "/usr/local/lib64/vst3", "/usr/local/lib/vst3"}) paths.emplace_back(path);
    if (!applicationFolder.empty()) paths.push_back((fs::path(applicationFolder) / "vst3").string());
    return paths;
}

BundlePaths discoverBundles(const std::vector<std::string>& roots, const std::atomic<bool>* cancel)
{
    BundlePaths result;
    const auto error = [&](const fs::path& path, const std::string& text) {
        if (result.errors.size() < 128) result.errors.push_back(path.string().substr(0, 512) + ": " + text);
    };
    if (roots.size() > 64) { result.limited = true; result.errors.emplace_back("VST3 search has too many roots"); return result; }
    std::set<std::string> visited, bundles;
    std::size_t entries = 0;
    for (const auto& root : roots) {
        if (root.empty() || root.size() > 4096 || root.find('\0') != root.npos) { error({}, "invalid VST3 search path"); continue; }
        std::vector<std::pair<fs::path, unsigned>> pending{{root, 0}};
        std::vector<std::string> found;
        while (!pending.empty()) {
            if (cancelled(cancel)) { result.paths.clear(); result.cancelled = true; return result; }
            auto [path, depth] = std::move(pending.back()); pending.pop_back();
            if (++entries > 100000 || depth > 32 || bundles.size() == kScanClasses) {
                result.limited = true; error(path, "VST3 search exceeds its tree or bundle limit"); return result;
            }
            std::error_code ec;
            if (!fs::is_directory(path, ec)) { if (ec && ec != std::errc::no_such_file_or_directory) error(path, ec.message()); continue; }
            const auto canonical = fs::canonical(path, ec);
            if (ec) { error(path, ec.message()); continue; }
            if (!visited.insert(canonical.string()).second) continue;
            if (canonical.extension() == ".vst3") {
                if (bundles.insert(canonical.string()).second) found.push_back(canonical.string());
                continue; // never descend into a plugin bundle
            }
            std::vector<fs::path> children;
            for (fs::directory_iterator it(canonical, ec), end; !ec && it != end; it.increment(ec)) {
                if (cancelled(cancel)) { result.paths.clear(); result.cancelled = true; return result; }
                if (++entries > 100000) { result.limited = true; error(canonical, "VST3 search exceeds its entry limit"); return result; }
                children.push_back(it->path());
            }
            if (ec) error(canonical, ec.message());
            std::sort(children.rbegin(), children.rend());
            for (auto& child : children) pending.emplace_back(std::move(child), depth + 1);
        }
        std::sort(found.begin(), found.end());
        result.paths.insert(result.paths.end(), found.begin(), found.end());
    }
    return result;
}
}
