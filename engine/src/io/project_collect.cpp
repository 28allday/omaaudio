#include "project_collect.h"
#include "project_storage.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <unistd.h>

namespace omawave::io {
namespace {
namespace fs = std::filesystem;

class ParentDirectory {
public:
    explicit ParentDirectory(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open collection destination parent");
    }
    ~ParentDirectory() { (void)::close(fd_); }
    ParentDirectory(const ParentDirectory&) = delete;
    ParentDirectory& operator=(const ParentDirectory&) = delete;
    int fd() const { return fd_; }
    fs::path path() const { return "/proc/self/fd/" + std::to_string(fd_); }
private:
    int fd_;
};

class Staging {
public:
    explicit Staging(const fs::path& parent) : path_((parent / ".omaaudio-collect-XXXXXX").string()) {
        if (!::mkdtemp(path_.data())) throw std::system_error(errno, std::generic_category(), "create project staging directory");
    }
    ~Staging() { std::error_code ec; if (!path_.empty()) fs::remove_all(path_, ec); }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    const std::string& path() const { return path_; }
    void published() { path_.clear(); }
private:
    std::string path_;
};

std::string videoName(const std::string& path)
{
    const auto id = session::Uuid::generate();
    if (id.isNull()) throw std::runtime_error("could not generate a reference media identifier");
    auto extension = fs::path(path).extension().string();
    if (extension.size() < 2 || extension.size() > 17 || !std::all_of(extension.begin() + 1, extension.end(),
            [](unsigned char c) { return std::isalnum(c); })) extension = ".video";
    return "media/" + id.toString() + extension;
}

std::string inventory(const session::Project& project, std::size_t& count)
{
    toml::table table{{"formatVersion", 1}, {"pluginsBundled", false},
        {"scope", "Insert requirements captured when this project was collected. Install compatible plugins separately; versions and availability are not verified."}};
    toml::array entries;
    const auto append = [&](const auto& inserts, const std::string& target, std::int64_t trackId,
                            const std::string& name, std::int64_t clipIndex = -1) {
        for (std::size_t slot = 0; slot < inserts.size(); ++slot) {
            const auto& insert = inserts[slot];
            const auto* format = insert.uri.starts_with("vst3:") ? "VST3" : insert.uri.starts_with("clap:") ? "CLAP" : "LV2";
            toml::table entry{{"uri", insert.uri}, {"format", format},
                {"target", target}, {"trackId", trackId}, {"trackName", name}, {"slot", static_cast<std::int64_t>(slot)},
                {"bypass", insert.bypass}, {"legacyClapControls", insert.legacyClapControls}};
            if (clipIndex >= 0) entry.insert("clipIndex", clipIndex);
            entries.push_back(std::move(entry));
            ++count;
        }
    };
    for (const auto& track : project.tracks()) append(track.inserts, "track", static_cast<std::int64_t>(track.id), track.name);
    append(project.masterInserts(), "master", -1, "Master");
    // Split views share one processing region. List its inserts once, using the
    // first view's index to locate it in the collected timeline.
    std::unordered_set<session::EffectOwner> clipOwners;
    for (const auto& track : project.tracks()) for (std::size_t index = 0; index < track.clips.size(); ++index) {
        const auto& clip = track.clips[index];
        if (clip.rack && clipOwners.insert(clip.rack->owner).second)
            append(clip.rack->inserts, "clip", static_cast<std::int64_t>(track.id), track.name,
                   static_cast<std::int64_t>(index));
    }
    table.insert("plugin", std::move(entries));
    std::ostringstream out; out << table << '\n';
    return out.str();
}
}

CollectResult collectProject(const session::Project& project, const ProjectView& view,
                             const std::string& path, const SaveOptions& options)
{
    CollectResult result;
    const auto cancelled = [&] {
        if (!options.cancel || !options.cancel->load(std::memory_order_relaxed)) return false;
        result.cancelled = true; result.error = "project collection cancelled"; return true;
    };
    try {
        if (cancelled()) return result;
        const fs::path requested(path);
        const auto name = requested.filename();
        if (path.empty() || path.find('\0') != std::string::npos || name.empty() || name == "." || name == "..") {
            result.error = "choose a new project directory"; return result;
        }
        // Canonicalize the parent by following the filesystem, preserving the
        // meaning of symlink/.. paths rather than simplifying them lexically.
        const fs::path parent = fs::canonical(requested.parent_path().empty() ? fs::path(".") : requested.parent_path());
        const ParentDirectory pinned(parent);
        const fs::path target = pinned.path() / name;
        result.path = (parent / name).string();
        std::error_code ec;
        const auto status = fs::symlink_status(target, ec);
        if ((ec && ec != std::errc::no_such_file_or_directory) || status.type() != fs::file_type::not_found) {
            result.error = "collection needs a new destination; already present or inaccessible: " + result.path; return result;
        }
        Staging stage(pinned.path());
        session::Project snapshot = project;
        if (!snapshot.video().empty()) snapshot.video().bundlePath = videoName(snapshot.video().path);
        SaveOptions save = options;
        save.buildPeakCaches = false; // Regenerable; avoid another full scan of every source.
        const auto saved = saveProject(snapshot, view, stage.path(), Layer::Project, save);
        if (!saved.ok) { result.error = saved.error; result.cancelled = saved.cancelled; return result; }
        if (cancelled()) return result;
        if (options.progress) options.progress("inventory", 0);
        const auto requirements = inventory(snapshot, result.plugins);
        if (cancelled()) return result;
        if (auto error = storage::writeText(fs::path(stage.path()) / "requirements.toml", requirements); !error.empty()) {
            result.error = error; return result;
        }
        if (auto error = storage::syncPath(stage.path(), true); !error.empty()) { result.error = error; return result; }
        if (options.progress) options.progress("publish", 0);
        if (cancelled()) return result;
        // Linux atomic no-replacement rename also protects empty directories and
        // dangling symlinks that appeared after the initial existence check.
        result.path = (fs::canonical(pinned.path()) / name).string();
        if (::renameat2(AT_FDCWD, stage.path().c_str(), pinned.fd(), name.c_str(), RENAME_NOREPLACE) != 0) {
            result.error = "could not publish collected project: " + std::error_code(errno, std::generic_category()).message(); return result;
        }
        stage.published();
        result.published = true;
        result.sources = std::count_if(snapshot.sources().begin(), snapshot.sources().end(), [](const auto& source) { return bool(source); });
        result.video = !snapshot.video().empty();
        if (auto error = storage::syncPath(pinned.path(), true); !error.empty()) { result.error = error; return result; }
        result.ok = true;
        if (options.progress) options.progress("publish", 1);
    } catch (const std::exception& exception) {
        result.error = exception.what();
        (void)cancelled();
    }
    return result;
}

} // namespace omawave::io
