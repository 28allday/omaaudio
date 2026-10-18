#include "project_file.h"
#include "session/effect_racks.h"
#include "session/elastic.h"
#include "session/clip_envelope.h"
#include "session/sample_edit.h"
#include "session/routing.h"

#include "io/audio_file.h"
#include "io/peak_cache.h"
#include "io/project_storage.h"
#include "io/plugin_state.h"
#include "session/track.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sstream>
#include <system_error>

namespace omawave::io {
namespace {

namespace fs = std::filesystem;

// Only one file beneath media/ is a portable reference. Do not permit absolute
// names, traversal or symlinks to turn a collected reference back into an external one.
std::string portableVideoPath(const fs::path& root, const std::string& relative, std::string& error)
{
    const fs::path name(relative);
    if (relative.find('\0') != std::string::npos || relative.find('\\') != std::string::npos
        || name.parent_path() != "media" || name.filename().empty()
        || name.filename() == "." || name.filename() == "..") {
        error = "invalid bundle-relative video path"; return {};
    }
    for (const auto& path : {root / "media", root / name}) {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if ((ec && ec != std::errc::no_such_file_or_directory) || fs::is_symlink(status)
            || (fs::exists(status) && (path == root / "media" ? !fs::is_directory(status) : !fs::is_regular_file(status)))) {
            error = "bundle-relative video must not use symlinks: " + path.string(); return {};
        }
    }
    std::error_code ec;
    const auto absolute = fs::canonical(root, ec) / name;
    if (ec) { error = "could not resolve project directory: " + ec.message(); return {}; }
    return absolute.string();
}

toml::table routingToToml(const session::TrackRouting& route)
{
    toml::table table;
    if (route.output == session::kInvalidTrack) table.insert("output", "master");
    else if (route.output == session::kNoTrackOutput) table.insert("output", "none");
    else table.insert("output", static_cast<std::int64_t>(route.output));
    if (route.sidechain != session::kInvalidTrack) table.insert("sidechain", static_cast<std::int64_t>(route.sidechain));
    toml::array sends;
    for (const auto& send : route.sends) sends.push_back(toml::table{
        {"target", static_cast<std::int64_t>(send.target)}, {"gain", static_cast<double>(send.gain)}, {"preFader", send.preFader}});
    if (!sends.empty()) table.insert("send", std::move(sends));
    return table;
}

bool routingFromToml(const toml::node* node, session::TrackRouting& route, int version)
{
    if (!node) return true;
    const auto* table = node->as_table();
    if (!table || version < kProjectFormatVersionRouting) return false;
    const auto id = [](const toml::node* field) -> std::optional<session::TrackId> {
        if (!field || !field->is_integer()) return {};
        const auto raw = field->value<std::int64_t>();
        if (!raw || *raw < 0 || *raw >= static_cast<std::int64_t>(session::kNoTrackOutput)) return {};
        return static_cast<session::TrackId>(*raw);
    };
    if (const auto output = (*table)["output"].value<std::string>()) {
        if (*output == "master") route.output = session::kInvalidTrack;
        else if (*output == "none") route.output = session::kNoTrackOutput;
        else return false;
    } else {
        const auto destination = id(table->get("output")); if (!destination) return false; route.output = *destination;
    }
    if (const auto* key = table->get("sidechain")) {
        const auto source = id(key); if (!source) return false; route.sidechain = *source;
    }
    if (const auto* node = table->get("send")) {
        const auto* sends = node->as_array(); if (!sends || sends->size() > session::kMaxTrackSends) return false;
        for (const auto& node : *sends) {
            const auto* send = node.as_table(); if (!send) return false;
            const auto target = id(send->get("target")); const auto gain = (*send)["gain"].value<double>();
            const auto pre = (*send)["preFader"].value<bool>();
            if (!target || !gain || !std::isfinite(*gain) || *gain < 0 || *gain > 4 || !pre) return false;
            route.sends.push_back({*target, static_cast<float>(*gain), *pre});
        }
    }
    return true;
}

// toml++ ships on this distribution as a shared library built with exceptions on, so
// TOML_EXCEPTIONS cannot be turned off without rebuilding it. The rest of the engine
// reports failure by return value (io/audio_file.h explains why there is one error
// convention rather than two), so every throw is caught here, at the only place in the
// engine that can produce one, and turned back into a return value.
struct ParsedToml {
    bool ok = false;
    std::string error;
    toml::table table;
};

// Every string as one escaped line. The formatter's own choice of a multi-line
// literal for text with a line break does not check for the three apostrophes
// that end one, and so can write a file that cannot be read back.
[[nodiscard]] std::string tomlText(const toml::table& table)
{
    std::ostringstream text;
    text << toml::toml_formatter{table, toml::toml_formatter::default_flags
        & ~toml::format_flags::allow_literal_strings & ~toml::format_flags::allow_multi_line_strings} << '\n';
    return text.str();
}

[[nodiscard]] ParsedToml parseTomlFile(const fs::path& path)
{
    ParsedToml out;
    try {
        out.table = toml::parse_file(path.string());
        out.ok = true;
    } catch (const toml::parse_error& e) {
        out.error = std::string(e.description());
        const toml::source_region& where = e.source();
        out.error += " (line " + std::to_string(where.begin.line) + ")";
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

[[nodiscard]] std::string nowIso8601()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (gmtime_r(&t, &utc) == nullptr)
        return {};
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        return {};
    return buffer;
}

// The journal publishes BOTH tables with one rename. Until its durable removal,
// readers use it instead of a possibly mixed pair of ordinary files.
constexpr auto kPendingSave = ".save-pending.toml";

struct Metadata {
    bool ok = false;
    std::string error;
    toml::table manifest;
    toml::table session;
};

[[nodiscard]] Metadata readMetadata(const fs::path& where)
{
    Metadata out;
    std::error_code ec;
    const bool pending = fs::exists(where / kPendingSave, ec);
    if (ec) { out.error = "could not check pending save: " + ec.message(); return out; }
    if (pending) {
        const auto journal = parseTomlFile(where / kPendingSave);
        const auto* manifest = journal.table["manifest"].as_table();
        const auto* session = journal.table["session"].as_table();
        if (!journal.ok || !manifest || !session
            || !(*manifest)["formatVersion"].value<std::int64_t>()
            || !(*manifest)["sampleRate"].value<std::int64_t>()
            || !(*session)["source"].is_array() || !(*session)["track"].is_array()) {
            out.error = "could not read pending save: " + journal.error;
            return out;
        }
        out.manifest = *manifest;
        out.session = *session;
    } else {
        const auto manifest = parseTomlFile(where / "manifest.toml");
        if (!manifest.ok) { out.error = "could not read manifest.toml: " + manifest.error; return out; }
        const auto session = parseTomlFile(where / "session.toml");
        if (!session.ok) { out.error = "could not read session.toml: " + session.error; return out; }
        out.manifest = manifest.table;
        out.session = session.table;
    }
    out.ok = true;
    return out;
}

[[nodiscard]] std::string finishPendingSave(const fs::path& where)
{
    std::error_code ec;
    const bool pending = fs::exists(where / kPendingSave, ec);
    if (ec) return "could not check pending save: " + ec.message();
    if (!pending) return {};
    const auto data = readMetadata(where);
    if (!data.ok) return data.error;
    for (const auto& name : {"manifest", "session"}) {
        const auto text = tomlText(std::string_view(name) == "manifest" ? data.manifest : data.session);
        if (auto error = storage::writeText(where / (std::string(name) + ".toml"), text);
            !error.empty()) return error;
    }
    return storage::removeFile(where / kPendingSave);
}

bool elasticFromToml(const toml::table& entry, session::Clip& clip, std::int64_t version) {
    const auto* node = entry.get("elastic");
    if (!node) return true;
    const auto* table = node->as_table();
    if (!table || version < kProjectFormatVersionElastic) return false;
    for (const char* key : {"timelineStart","sourceOffset","length"})
        if (!entry[key].is_integer()) return false;
    if ((*table)["processor"].value_or(std::string{}) != "signalsmith-v1") return false;
    const auto start = (*table)["inputStart"].value<std::int64_t>();
    const auto input = (*table)["inputFrames"].value<std::int64_t>();
    const auto output = (*table)["outputFrames"].value<std::int64_t>();
    const auto pitch = (*table)["semitones"].value<double>();
    if (!start || !input || !output || !pitch || !std::isfinite(*pitch) || *pitch < -12 || *pitch > 12) return false;
    clip.elastic = session::ElasticSpec{*start,*input,*output,float(*pitch)};
    return clip.elastic->valid();
}

bool shapedFades(const session::Clip& clip)
{
    return clip.fadeInShape != session::FadeShape::Linear || clip.fadeOutShape != session::FadeShape::Linear
        || clip.fadeInCurve != 0 || clip.fadeOutCurve != 0;
}

bool fadeShapesFromToml(const toml::table& entry, session::Clip& clip, std::int64_t version)
{
    for (const auto [name, value] : {std::pair{"fadeInShape", &clip.fadeInShape},
                                    {"fadeOutShape", &clip.fadeOutShape}}) {
        const auto field = entry[name];
        if (!field) continue;
        const auto text = field.value<std::string>();
        if (version < kProjectFormatVersionFadeShapes || !text || (*text != "linear" && *text != "smooth")) return false;
        *value = *text == "smooth" ? session::FadeShape::Smooth : session::FadeShape::Linear;
    }
    for (const auto [name, value] : {std::pair{"fadeInCurve", &clip.fadeInCurve},
                                    {"fadeOutCurve", &clip.fadeOutCurve}}) {
        const auto field = entry[name];
        if (!field) continue;
        const auto number = field.value<double>();
        if (version < kProjectFormatVersionFadeShapes || !field.is_number() || !number
            || !std::isfinite(*number) || *number < -1 || *number > 1) return false;
        *value = static_cast<float>(*number);
    }
    return clip.hasValidFades();
}

toml::table automationToToml(const session::AutomationCurve&);
bool automationFromToml(const toml::node*, session::AutomationCurve&, double, double, std::int64_t, std::string&);
bool clipEnvelopesFromToml(const toml::table& entry, session::Clip& clip, std::int64_t version)
{
    if (const auto field = entry["envelopeOffset"]; field) {
        if (version < kProjectFormatVersionClipEnvelopes || !field.is_integer()) return false;
        clip.envelopeOffset = field.value_or<std::int64_t>(-1);
    }
    for (const auto [name,curve] : {std::pair{"volumeEnvelope",&clip.volumeEnvelope},
                                    {"panEnvelope",&clip.panEnvelope}}) {
        const auto* node = entry.get(name); if (!node) continue;
        if (version < kProjectFormatVersionClipEnvelopes) return false;
        std::string error;
        const bool volume = curve == &clip.volumeEnvelope;
        if (!automationFromToml(node,*curve,volume ? 0 : -1,volume ? 4 : 1,version,error)) return false;
        const auto* points = (*node->as_table())["point"].as_array();
        for (const auto& point : *points) if (!(*point.as_table())["frame"].is_integer()) return false;
    }
    return clip.hasValidEnvelopes();
}

// Live clip effect regions, numbered in first-use order. Split views share one.
using ClipRackIds = std::vector<const session::ClipRack*>;

[[nodiscard]] toml::array clipsToToml(const std::vector<session::Clip>& clips,
                                      const session::Project& project, bool groups = true,
                                      ClipRackIds* racks = nullptr)
{
    toml::array out;
    for (const session::Clip& c : clips) {
        const session::AudioSource* source = project.source(c.source);
        if (source == nullptr)
            continue;                       // a clip nothing can resolve is not a clip

        toml::table entry;
        // By uuid, not by SourceId: an id is an index into this run's source list and
        // means nothing in a file.
        entry.insert("source", source->uuid.toString());
        entry.insert("timelineStart", static_cast<std::int64_t>(c.timelineStart));
        entry.insert("sourceOffset", static_cast<std::int64_t>(c.sourceOffset));
        entry.insert("length", static_cast<std::int64_t>(c.length));
        entry.insert("gain", static_cast<double>(c.gain));
        if (c.volumeEnvelope.hasData()) entry.insert("volumeEnvelope",automationToToml(c.volumeEnvelope));
        if (c.panEnvelope.hasData()) entry.insert("panEnvelope",automationToToml(c.panEnvelope));
        if (c.envelopeOffset) entry.insert("envelopeOffset",c.envelopeOffset);
        entry.insert("fadeIn", static_cast<std::int64_t>(c.fadeIn));
        entry.insert("fadeOut", static_cast<std::int64_t>(c.fadeOut));
        if (c.fadeInShape != session::FadeShape::Linear) entry.insert("fadeInShape", "smooth");
        if (c.fadeOutShape != session::FadeShape::Linear) entry.insert("fadeOutShape", "smooth");
        if (c.fadeInCurve != 0) entry.insert("fadeInCurve", double(c.fadeInCurve));
        if (c.fadeOutCurve != 0) entry.insert("fadeOutCurve", double(c.fadeOutCurve));
        if (groups && c.group != session::kInvalidClipGroup)
            entry.insert("group", std::int64_t(c.group));
        if (c.fadeInOffset != 0)
            entry.insert("fadeInOffset", static_cast<std::int64_t>(c.fadeInOffset));
        if (c.fadeOutOffset != 0)
            entry.insert("fadeOutOffset", static_cast<std::int64_t>(c.fadeOutOffset));
        if (c.elastic) {
            const auto& e = *c.elastic;
            entry.insert("elastic",toml::table{{"processor","signalsmith-v1"},
                {"inputStart",e.inputStart},{"inputFrames",e.inputFrames},
                {"outputFrames",e.outputFrames},{"semitones",double(e.semitones)}});
        }
        if (racks && c.rack) {
            auto found = std::find(racks->begin(), racks->end(), c.rack.get());
            if (found == racks->end()) found = racks->insert(racks->end(), c.rack.get());
            entry.insert("rack", static_cast<std::int64_t>(found - racks->begin()));
            if (c.effectOffset) entry.insert("effectOffset", static_cast<std::int64_t>(c.effectOffset));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::optional<session::CrossfadeRecipe> crossfadeFromToml(const toml::table& entry,
    const session::Project& project, std::size_t frames, std::uint32_t channels, std::int64_t version)
{
    const auto* table = entry["crossfade"].as_table();
    if (!table) return {};
    const auto start = (*table)["start"].value<std::int64_t>();
    const auto end = (*table)["end"].value<std::int64_t>();
    const auto power = (*table)["equalPower"].value<bool>();
    const auto* pair = (*table)["clip"].as_array();
    if (!start || !end || !power || !pair || pair->size() != 2) return {};
    session::CrossfadeRecipe recipe{.start = *start, .end = *end, .equalPower = *power};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* c = (*pair)[i].as_table();
        if (!c) return {};
        const auto uuidText = (*c)["source"].value<std::string>();
        const auto uuid = uuidText ? session::Uuid::parse(*uuidText) : std::nullopt;
        const auto source = uuid ? project.sourceIdByUuid(*uuid) : std::nullopt;
        if (!source) return {};
        auto& clip = i == 0 ? recipe.left : recipe.right;
        clip.source = *source;
        for (auto [name, value] : {std::pair{"timelineStart", &clip.timelineStart},
            {"sourceOffset", &clip.sourceOffset}, {"length", &clip.length},
            {"fadeIn", &clip.fadeIn}, {"fadeOut", &clip.fadeOut},
            {"fadeInOffset", &clip.fadeInOffset}, {"fadeOutOffset", &clip.fadeOutOffset}}) {
            const auto field = (*c)[name];
            if (field && !field.is_integer()) return {};
            *value = field.value_or<std::int64_t>(0);
        }
        const auto gain = (*c)["gain"].value<double>();
        if (!gain) return {};
        clip.gain = static_cast<float>(*gain);
        if (!fadeShapesFromToml(*c, clip, version) || !elasticFromToml(*c,clip,version)
            || !clipEnvelopesFromToml(*c,clip,version)) return {};
    }
    return session::validCrossfadeRecipe(project, recipe, frames, channels)
        ? std::optional{recipe} : std::nullopt;
}

toml::table automationToToml(const session::AutomationCurve& curve)
{
    toml::table result;
    result.insert("enabled", curve.enabled); result.insert("hold", curve.hold);
    toml::array points;
    for (const auto& p : curve.points) {
        toml::table point{{"frame", static_cast<std::int64_t>(p.frame)}, {"value", static_cast<double>(p.value)}};
        if (p.interpolation != session::AutomationInterpolation::Default)
            point.insert("interpolation", p.interpolation == session::AutomationInterpolation::Smooth ? "smooth"
                : p.interpolation == session::AutomationInterpolation::Linear ? "linear" : "hold");
        if (p.interpolation == session::AutomationInterpolation::Smooth
            && (p.smoothStart != 0 || p.smoothEnd != 1)) {
            point.insert("smoothStart", p.smoothStart);
            point.insert("smoothEnd", p.smoothEnd);
        }
        points.push_back(std::move(point));
    }
    result.insert("point", std::move(points));
    return result;
}

bool automationFromToml(const toml::node* node, session::AutomationCurve& curve,
                        double minimum, double maximum, std::int64_t version, std::string& error)
{
    if (!node) return true;
    const auto fail = [&] { error = "invalid automation: expected ordered unique frames, finite in-range values and at most 4096 points; linear/hold segments need format 18, smooth segments need format 23 and finite ordered phase bounds within 0..1"; return false; };
    if (version < kProjectFormatVersionAutomation) return fail();
    const auto* table = node->as_table();
    if (!table) return fail();
    const auto enabled = (*table)["enabled"].value<bool>(), hold = (*table)["hold"].value<bool>();
    const auto* points = (*table)["point"].as_array();
    if (!enabled || !hold || !points || points->size() > session::AutomationCurve::kMaxPoints) return fail();
    curve.enabled = *enabled; curve.hold = *hold;
    for (const auto& n : *points) {
        const auto* point = n.as_table(); if (!point) return fail();
        const auto frame = (*point)["frame"].value<std::int64_t>();
        const auto value = (*point)["value"].value<double>();
        if (!frame || !value || !std::isfinite(*value) || *value < minimum || *value > maximum) return fail();
        auto interpolation = session::AutomationInterpolation::Default;
        if (const auto field = (*point)["interpolation"]) {
            const auto mode = field.value<std::string>();
            if (version < kProjectFormatVersionAutomationSegments || !mode
                || (*mode != "linear" && *mode != "hold" && *mode != "smooth")) return fail();
            if (*mode == "smooth" && version < kProjectFormatVersionSmoothAutomation) return fail();
            interpolation = *mode == "smooth" ? session::AutomationInterpolation::Smooth
                : *mode == "linear" ? session::AutomationInterpolation::Linear : session::AutomationInterpolation::Hold;
        }
        session::AutomationPoint parsed{*frame, version >= kProjectFormatVersionPluginState ? *value : static_cast<float>(*value), interpolation};
        const auto start = (*point)["smoothStart"], end = (*point)["smoothEnd"];
        if (start || end) {
            if (version < kProjectFormatVersionSmoothAutomation || interpolation != session::AutomationInterpolation::Smooth
                || !start.is_number() || !end.is_number()) return fail();
            const auto from = start.value<double>(), to = end.value<double>();
            if (!from || !to || !std::isfinite(*from) || !std::isfinite(*to)
                || *from < 0 || *to > 1 || *from >= *to) return fail();
            parsed.smoothStart = *from; parsed.smoothEnd = *to;
        }
        curve.points.push_back(parsed);
    }
    return curve.valid(minimum, maximum) || fail();
}

[[nodiscard]] toml::array insertsToToml(const std::vector<session::Insert>& inserts)
{
    toml::array out;
    for (const session::Insert& insert : inserts) {
        toml::table entry;
        entry.insert("uri", insert.uri);
        entry.insert("bypass", insert.bypass);
        if (insert.uri.starts_with("clap:") && !insert.legacyClapControls)
            entry.insert("clapParamIds", true);
        if (!insert.instanceState.empty()) {
            toml::array states;
            for (const auto& state : insert.instanceState) states.push_back(state.id().toString());
            entry.insert("instanceState", std::move(states));
        }
        // Only the controls that were moved: the plugin's own defaults are not ours
        // to restate, and one installed here has a thousand ports.
        toml::array controls;
        for (const session::InsertControl& c : insert.controls) {
            toml::table control;
            control.insert("port", static_cast<std::int64_t>(c.port));
            control.insert("value", static_cast<double>(c.value));
            if (c.fromState) control.insert("fromState", true);
            if (c.automation.hasData()) control.insert("automation", automationToToml(c.automation));
            if (c.hasInstanceAutomation()) {
                toml::array instances;
                for (std::size_t actual = 0; actual < c.instanceAutomation.size(); ++actual)
                    if (const auto& instance = c.instanceAutomation[actual])
                        instances.push_back(toml::table{{"instance", static_cast<std::int64_t>(actual)},
                            {"base", instance->base}, {"automation", automationToToml(instance->curve)}});
                control.insert("instanceAutomation", std::move(instances));
            }
            controls.push_back(std::move(control));
        }
        entry.insert("control", std::move(controls));
        out.push_back(std::move(entry));
    }
    return out;
}

class StateLoader {
public:
    explicit StateLoader(const fs::path& root) : root_(root) {}
    bool load(const toml::node* node, session::Insert& insert, std::int64_t version, std::string& error) {
        if (!node) return true;
        const auto* array = node->as_array();
        if (version < kProjectFormatVersionPluginState || !array || array->size() > 2
            || !session::PluginState::validUri(insert.uri)) {
            error = "plugin instance states need format 16, a plugin URI and at most two references"; return false;
        }
        for (const auto& entry : *array) {
            const auto text = entry.value<std::string>();
            const auto id = text ? session::Uuid::parse(*text) : std::nullopt;
            if (!id || id->isNull() || id->toString() != *text) {
                error = "plugin state reference needs a canonical nonzero UUID"; return false;
            }
            auto found = cache_.find(id->bytes);
            if (found == cache_.end()) {
                auto state = readPluginState(root_, *id, insert.uri,
                    session::PluginState::kProjectLimit - budget_.bytes(), error);
                if (!state) return false;
                found = cache_.emplace(id->bytes, std::move(*state)).first;
            }
            if (found->second.uri() != insert.uri) { error = "plugin state reference belongs to another plugin"; return false; }
            if (!budget_.add(found->second, error)) return false;
            insert.instanceState.push_back(found->second);
        }
        return true;
    }
private:
    const fs::path& root_;
    session::PluginStateBudget budget_;
    std::map<std::array<std::uint8_t,16>, session::PluginState> cache_;
};

[[nodiscard]] std::vector<session::Insert> insertsFromToml(const toml::node* declaration,
    std::int64_t version, StateLoader& states, std::string& error)
{
    std::vector<session::Insert> out;
    const auto* inserts = declaration ? declaration->as_array() : nullptr;
    if (declaration && !inserts && version >= kProjectFormatVersionPluginState) {
        error = "invalid plugin insert declaration"; return {};
    }
    if (inserts == nullptr)
        return out;
    for (const toml::node& node : *inserts) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            if (version >= kProjectFormatVersionPluginState) { error = "invalid plugin insert entry"; return {}; }
            continue;
        }
        session::Insert insert;
        insert.uri = (*entry)["uri"].value_or(std::string{});
        if (!states.load(entry->get("instanceState"), insert, version, error)) return {};
        const bool vst3 = insert.uri.starts_with("vst3:");
        const double controlLimit = version >= kProjectFormatVersionPluginState && insert.uri.starts_with("clap:")
            ? std::numeric_limits<double>::max() : std::numeric_limits<float>::max();
        if (vst3 && version < kProjectFormatVersionPluginState) { error = "VST3 parameters need project format 16"; return {}; }
        if (insert.uri.empty())
            continue;               // an insert with no identity is nothing
        insert.bypass = (*entry)["bypass"].value_or<bool>(false);
        if (insert.uri.starts_with("clap:")) {
            const bool ids = (*entry)["clapParamIds"].value_or<bool>(false);
            if (ids && version < kProjectFormatVersionClapIds) {
                error = "stable CLAP parameter IDs need project format 13"; return {};
            }
            insert.legacyClapControls = !ids;
        }
        if (entry->contains("control") && !(*entry)["control"].is_array() && version >= kProjectFormatVersionPluginState) {
            error = "invalid plugin control declaration"; return {};
        }
        if (const toml::array* controls = (*entry)["control"].as_array()) {
            std::unordered_set<std::uint32_t> seen;
            for (const toml::node& cn : *controls) {
                const toml::table* control = cn.as_table();
                if (control == nullptr) {
                    if (version >= kProjectFormatVersionPluginState) { error = "invalid plugin control entry"; return {}; }
                    continue;
                }
                const auto port = (*control)["port"].value<std::int64_t>();
                const auto value = (*control)["value"].value<double>();
                const auto* automation = control->get("automation");
                const auto* stateControl = control->get("fromState");
                if (stateControl && (version < kProjectFormatVersionStateControls || !stateControl->is_boolean()
                    || (stateControl->value_or(false) && insert.instanceState.empty()))) {
                    error = "state-authoritative controls need format 17, a boolean and a plugin state reference"; return {};
                }
                if (version >= kProjectFormatVersionAutomation || automation) {
                    if (!port || !value || *port < 0 || *port > std::numeric_limits<std::uint32_t>::max()
                        || !std::isfinite(*value) || std::abs(*value) > controlLimit
                        || !seen.insert(static_cast<std::uint32_t>(*port)).second) {
                        error = "invalid automation control port/value or duplicate port"; return {};
                    }
                    if (vst3 && (*port > 0x7FFFFFFF || *value < 0 || *value > 1)) {
                        error = "invalid VST3 parameter ID or normalized value"; return {};
                    }
                }
                if (port.has_value() && value.has_value() && *port >= 0) {
                    insert.setControl(static_cast<std::uint32_t>(*port),
                        version >= kProjectFormatVersionPluginState ? *value : static_cast<float>(*value));
                    insert.controls.back().fromState = stateControl && stateControl->value_or(false);
                    if (!automationFromToml(automation, insert.controls.back().automation,
                            vst3 ? 0 : -controlLimit, vst3 ? 1 : controlLimit, version, error)) return {};
                    if (const auto* declaration = control->get("instanceAutomation")) {
                        const auto* instances = declaration->as_array();
                        const auto invalid = [&] {
                            error = "instance automation needs format 19, unique instance indices 0 or 1, finite in-range bases and valid curves";
                        };
                        if (version < kProjectFormatVersionInstanceAutomation || !instances || instances->empty() || instances->size() > 2) {
                            invalid(); return {};
                        }
                        auto& overrides = insert.controls.back().instanceAutomation;
                        for (const auto& item : *instances) {
                            const auto* instance = item.as_table();
                            if (!instance) { invalid(); return {}; }
                            const auto actual = (*instance)["instance"].value<std::int64_t>();
                            const auto base = (*instance)["base"].value<double>();
                            const auto* curve = instance->get("automation");
                            if (!actual || *actual < 0 || *actual > 1 || overrides[*actual] || !base || !std::isfinite(*base)
                                || *base < (vst3 ? 0 : -controlLimit) || *base > (vst3 ? 1 : controlLimit) || !curve) {
                                invalid(); return {};
                            }
                            session::InstanceAutomation parsed{*base, {}};
                            if (!automationFromToml(curve, parsed.curve, vst3 ? 0 : -controlLimit,
                                                    vst3 ? 1 : controlLimit, version, error)) return {};
                            overrides[*actual] = std::move(parsed);
                        }
                    }
                }
            }
        }
        out.push_back(std::move(insert));
    }
    return out;
}

// --- the Spectral page's state (A22) ----------------------------------------------

[[nodiscard]] toml::table spectralToToml(const ProjectView& view)
{
    toml::table out;
    out.insert("bandLowHz", static_cast<double>(view.band.lowHz));
    out.insert("bandHighHz", static_cast<double>(view.band.highHz));
    out.insert("maskFeather", static_cast<std::int64_t>(view.mask.featherBins));

    toml::array shapes;
    for (const spectral::MaskShape& shape : view.mask.shapes) {
        if (shape.empty())
            continue;
        toml::table entry;
        entry.insert("op", shape.op == spectral::MaskOp::Subtract ? "cut"
                           : shape.op == spectral::MaskOp::Intersect ? "keep" : "add");
        if (shape.comb.has_value()) {
            const spectral::HarmonicComb& comb = *shape.comb;
            entry.insert("kind", "comb");
            entry.insert("from", static_cast<double>(comb.fromSeconds));
            entry.insert("to", static_cast<double>(comb.toSeconds));
            entry.insert("hz", static_cast<double>(comb.fundamentalHz));
            entry.insert("partials", static_cast<std::int64_t>(comb.partials));
            entry.insert("width", static_cast<double>(comb.width));
            entry.insert("widthInHz", comb.widthInHz);
            entry.insert("fundamental", comb.includeFundamental);
            if (!comb.pitch.empty()) {
                toml::array pitch;
                for (const spectral::MaskPoint& p : comb.pitch)
                    pitch.push_back(toml::array{static_cast<double>(p.seconds), static_cast<double>(p.hz)});
                entry.insert("pitch", std::move(pitch));
            }
        } else if (!shape.polygon.empty()) {
            entry.insert("kind", "lasso");
            toml::array points;
            for (const spectral::MaskPoint& p : shape.polygon) {
                points.push_back(toml::array{static_cast<double>(p.seconds),
                                             static_cast<double>(p.hz)});
            }
            entry.insert("points", std::move(points));
        } else {
            entry.insert("kind", "stroke");
            toml::array stamps;
            for (const spectral::MaskStamp& st : shape.stamps) {
                stamps.push_back(toml::array{static_cast<double>(st.seconds),
                                             static_cast<double>(st.hz),
                                             static_cast<double>(st.rSeconds),
                                             static_cast<double>(st.hzLow),
                                             static_cast<double>(st.hzHigh)});
            }
            entry.insert("stamps", std::move(stamps));
        }
        shapes.push_back(std::move(entry));
    }
    if (!shapes.empty())
        out.insert("shape", std::move(shapes));

    const spectral::NoisePrint& print = view.noisePrint;
    if (!print.empty()) {
        toml::table entry;
        entry.insert("fftSize", static_cast<std::int64_t>(print.fftSize));
        entry.insert("bins", static_cast<std::int64_t>(print.bins));
        entry.insert("window", std::string(spectral::windowName(print.window)));
        entry.insert("columns", static_cast<std::int64_t>(print.columns));
        entry.insert("summary", view.noisePrintSummary);
        entry.insert("channelScope", print.channelScope);
        toml::array channels;
        for (const std::vector<float>& plane : print.magnitude) {
            toml::array bins;
            for (const float v : plane)
                bins.push_back(static_cast<double>(v));
            channels.push_back(std::move(bins));
        }
        entry.insert("magnitude", std::move(channels));
        out.insert("noisePrint", std::move(entry));
    }
    return out;
}

// A row of numbers, or nothing: a shape with a malformed point is dropped whole
// rather than drawn wrong.
[[nodiscard]] std::optional<std::vector<float>> floatRow(const toml::node& node, std::size_t count)
{
    const toml::array* row = node.as_array();
    if (row == nullptr || row->size() != count)
        return std::nullopt;
    std::vector<float> out;
    out.reserve(count);
    for (const toml::node& v : *row) {
        const auto value = v.value<double>();
        if (!value.has_value())
            return std::nullopt;
        out.push_back(static_cast<float>(*value));
    }
    return out;
}

bool spectralFromToml(const toml::table* table, ProjectView& view)
{
    if (table == nullptr)
        return true;
    view.band.lowHz = static_cast<float>((*table)["bandLowHz"].value_or<double>(0.0));
    view.band.highHz = static_cast<float>((*table)["bandHighHz"].value_or<double>(0.0));
    const auto feather = std::clamp<std::int64_t>((*table)["maskFeather"].value_or<std::int64_t>(2), 0, 64);
    view.mask.featherBins = static_cast<std::uint32_t>(feather);
    view.mask.featherColumns = static_cast<std::uint32_t>(feather);

    if (const toml::array* shapes = (*table)["shape"].as_array()) {
        for (const toml::node& node : *shapes) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;
            const std::string opName = (*entry)["op"].value_or(std::string{});
            const spectral::MaskOp op = opName == "cut" ? spectral::MaskOp::Subtract
                                      : opName == "keep" ? spectral::MaskOp::Intersect
                                                         : spectral::MaskOp::Add;
            const std::string kind = (*entry)["kind"].value_or(std::string{});
            if (kind == "lasso") {
                std::vector<spectral::MaskPoint> points;
                if (const toml::array* rows = (*entry)["points"].as_array()) {
                    for (const toml::node& r : *rows) {
                        if (const auto row = floatRow(r, 2))
                            points.push_back({(*row)[0], (*row)[1]});
                    }
                }
                if (points.size() >= 3)
                    view.mask.shapes.push_back(spectral::MaskShape::lasso(std::move(points), op));
            } else if (kind == "comb") {
                spectral::HarmonicComb comb;
                comb.fromSeconds = static_cast<float>((*entry)["from"].value_or<double>(0.0));
                comb.toSeconds = static_cast<float>((*entry)["to"].value_or<double>(0.0));
                comb.fundamentalHz = static_cast<float>((*entry)["hz"].value_or<double>(0.0));
                comb.partials = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>((*entry)["partials"].value_or<std::int64_t>(0), 0, 256));
                comb.width = static_cast<float>((*entry)["width"].value_or<double>(0.0));
                comb.widthInHz = (*entry)["widthInHz"].value_or(false);
                comb.includeFundamental = (*entry)["fundamental"].value_or(true);
                if ((*entry)["pitch"] && !(*entry)["pitch"].is_array())
                    return false;
                if (const toml::array* pitch = (*entry)["pitch"].as_array()) {
                    if (pitch->size() < 2 || pitch->size() > 4097)
                        return false;
                    for (const toml::node& point : *pitch) {
                        const auto row = floatRow(point, 2);
                        if (!row) return false;
                        comb.pitch.push_back({(*row)[0], (*row)[1]});
                    }
                }
                if (!comb.validPitch()) return false;
                if (!comb.empty() && comb.width > 0.0f)
                    view.mask.shapes.push_back(spectral::MaskShape::harmonics(comb, op));
            } else if (kind == "stroke") {
                std::vector<spectral::MaskStamp> stamps;
                if (const toml::array* rows = (*entry)["stamps"].as_array()) {
                    for (const toml::node& r : *rows) {
                        if (const auto row = floatRow(r, 5)) {
                            stamps.push_back({.seconds = (*row)[0], .hz = (*row)[1],
                                              .rSeconds = (*row)[2], .hzLow = (*row)[3],
                                              .hzHigh = (*row)[4]});
                        }
                    }
                }
                if (!stamps.empty())
                    view.mask.shapes.push_back(spectral::MaskShape::stroke(std::move(stamps), op));
            }
        }
    }

    if (const toml::table* entry = (*table)["noisePrint"].as_table()) {
        spectral::NoisePrint print;
        print.fftSize = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, (*entry)["fftSize"].value_or<std::int64_t>(0)));
        print.bins = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, (*entry)["bins"].value_or<std::int64_t>(0)));
        if (const auto w = spectral::windowFromName((*entry)["window"].value_or(std::string{})))
            print.window = *w;
        print.columns = static_cast<std::size_t>(
            std::max<std::int64_t>(0, (*entry)["columns"].value_or<std::int64_t>(0)));
        print.channelScope = (*entry)["channelScope"].value_or(std::string("all"));
        bool sound = print.fftSize >= 64 && print.bins == print.fftSize / 2 + 1
            && (print.channelScope == "all" || print.channelScope == "left" || print.channelScope == "right"
                || print.channelScope == "mid" || print.channelScope == "side");
        if (const toml::array* channels = (*entry)["magnitude"].as_array()) {
            for (const toml::node& plane : *channels) {
                const auto row = floatRow(plane, print.bins);
                if (!row.has_value()) {
                    sound = false;
                    break;
                }
                print.magnitude.push_back(*row);
            }
        }
        // A print whose numbers do not agree with each other is worse than none: it
        // would subtract the wrong spectrum from everything.
        if (sound && !print.magnitude.empty() && (print.channelScope == "all" || print.magnitude.size() == 1)) {
            view.noisePrint = std::move(print);
            view.noisePrintSummary = (*entry)["summary"].value_or(std::string{});
        }
    }
    return true;
}

[[nodiscard]] fs::path layerDir(const fs::path& root, Layer layer)
{
    return layer == Layer::Recovery ? root / "recovery" : root;
}

[[nodiscard]] bool anyInserts(const session::Project& project) noexcept
{
    for (const session::Track& t : project.tracks()) {
        if (!t.inserts.empty())
            return true;
    }
    return !project.masterInserts().empty();
}

[[nodiscard]] std::int64_t markerWire(session::MarkerId id) noexcept
{
    return id == session::kInvalidMarker
             ? -1
             : static_cast<std::int64_t>(static_cast<std::uint32_t>(id));
}

[[nodiscard]] std::int64_t trackWire(session::TrackId id) noexcept
{
    return id == session::kInvalidTrack
             ? -1
             : static_cast<std::int64_t>(static_cast<std::uint32_t>(id));
}

[[nodiscard]] session::TrackId trackFromWire(std::int64_t raw) noexcept
{
    return raw < 0 ? session::kInvalidTrack
                   : static_cast<session::TrackId>(static_cast<std::uint32_t>(raw));
}

} // namespace

bool isWholeSourceClip(const session::Project& project,
                       const std::vector<session::Clip>& clips)
{
    if (clips.size() != 1)
        return false;
    const session::Clip& c = clips.front();
    if (c.elastic || c.timelineStart != 0 || c.sourceOffset != 0)
        return false;
    if (c.gain != 1.0f || c.fadeIn != 0 || c.fadeOut != 0 || c.hasEnvelopes())
        return false;

    const session::AudioSource* source = project.source(c.source);
    if (source == nullptr)
        return false;
    return c.length == static_cast<session::FramePos>(source->audio.frames());
}

namespace {
toml::table bextToToml(const session::BroadcastInfo& b)
{
    toml::table t;
    t.insert("description", b.description);
    t.insert("originator", b.originator);
    t.insert("originatorReference", b.originatorReference);
    t.insert("originationDate", b.originationDate);
    t.insert("originationTime", b.originationTime);
    t.insert("timeReference", static_cast<std::int64_t>(b.timeReference));
    t.insert("version", static_cast<std::int64_t>(b.version));
    if (!b.umidHex.empty()) t.insert("umid", b.umidHex);
    if (!b.codingHistory.empty()) t.insert("codingHistory", b.codingHistory);
    if (b.loudnessValue) t.insert("loudnessValue", *b.loudnessValue);
    if (b.loudnessRange) t.insert("loudnessRange", *b.loudnessRange);
    if (b.maxTruePeak) t.insert("maxTruePeak", *b.maxTruePeak);
    if (b.maxMomentaryLoudness) t.insert("maxMomentaryLoudness", *b.maxMomentaryLoudness);
    if (b.maxShortTermLoudness) t.insert("maxShortTermLoudness", *b.maxShortTermLoudness);
    return t;
}
session::BroadcastInfo bextFromToml(const toml::table& t)
{
    session::BroadcastInfo b;
    b.present = true;
    b.description = t["description"].value_or(std::string{});
    b.originator = t["originator"].value_or(std::string{});
    b.originatorReference = t["originatorReference"].value_or(std::string{});
    b.originationDate = t["originationDate"].value_or(std::string{});
    b.originationTime = t["originationTime"].value_or(std::string{});
    b.timeReference = t["timeReference"].value_or(std::int64_t{0});
    b.version = static_cast<int>(t["version"].value_or(std::int64_t{0}));
    b.umidHex = t["umid"].value_or(std::string{});
    b.codingHistory = t["codingHistory"].value_or(std::string{});
    if (auto v = t["loudnessValue"].value<double>()) b.loudnessValue = *v;
    if (auto v = t["loudnessRange"].value<double>()) b.loudnessRange = *v;
    if (auto v = t["maxTruePeak"].value<double>()) b.maxTruePeak = *v;
    if (auto v = t["maxMomentaryLoudness"].value<double>()) b.maxMomentaryLoudness = *v;
    if (auto v = t["maxShortTermLoudness"].value<double>()) b.maxShortTermLoudness = *v;
    return b;
}
toml::table provenanceToToml(const session::AudioMetadata& m)
{
    toml::table t;
    if (m.bext.present) t.insert("bext", bextToToml(m.bext));
    if (!m.markers.empty()) {
        toml::array markers;
        for (const auto& marker : m.markers) {
            toml::table entry;
            entry.insert("position", static_cast<std::int64_t>(marker.position));
            entry.insert("length", static_cast<std::int64_t>(marker.length));
            entry.insert("label", marker.label);
            markers.push_back(std::move(entry));
        }
        t.insert("marker", std::move(markers));
    }
    if (!m.ixml.empty()) t.insert("ixml", m.ixml);
    if (!m.strings.empty()) {
        toml::table strings;
        for (const auto& [k, v] : m.strings) strings.insert(k, v);
        t.insert("strings", std::move(strings));
    }
    return t;
}
bool provenanceFromToml(const toml::table& t, session::AudioMetadata& m)
{
    if (const toml::table* bext = t["bext"].as_table()) m.bext = bextFromToml(*bext);
    if (const toml::array* markers = t["marker"].as_array()) {
        for (const toml::node& node : *markers) {
            const toml::table* entry = node.as_table();
            if (!entry) return false;
            session::FileMarker marker;
            marker.position = (*entry)["position"].value_or(std::int64_t{0});
            marker.length = (*entry)["length"].value_or(std::int64_t{0});
            marker.label = (*entry)["label"].value_or(std::string{});
            if (marker.position < 0 || marker.length < 0) return false;
            m.markers.push_back(std::move(marker));
        }
    }
    m.ixml = t["ixml"].value_or(std::string{});
    if (const toml::table* strings = t["strings"].as_table()) {
        for (const auto& [k, v] : *strings)
            if (auto text = v.value<std::string>()) m.strings[std::string(k.str())] = *text;
    }
    return true;
}
} // namespace

SaveResult saveProject(const session::Project& project, const ProjectView& view,
                       const std::string& path, Layer layer, const SaveOptions& options)
{
    SaveResult result;
    const auto cancelled = [&] {
        if (!options.cancel || !options.cancel->load(std::memory_order_relaxed)) return false;
        result.cancelled = true; result.error = "project save cancelled"; return true;
    };
    if (cancelled()) return result;
    // Live clip effect regions are stored (format 24). A crossfade recipe keeps
    // already rendered audio; retaining an effect version inside it is refused.
    bool clipEffects = false;
    for (const auto& track : project.tracks())
        for (const auto& clip : track.clips) clipEffects = clipEffects || bool(clip.rack);
    if (clipEffects) {
        if (const auto error = session::validateClipRacks(project); !error.empty()) { result.error = error; return result; }
    }
    for (const auto& source : project.sources()) if (source && source->crossfade
        && (source->crossfade->left.rack || source->crossfade->right.rack)) {
        result.error = "retained clip effect storage is not yet available"; return result;
    }
    if (project.sampleRate() < 8000 || project.sampleRate() > 192000
        || (project.defaultExportBits() != 16 && project.defaultExportBits() != 24 && project.defaultExportBits() != 32)) {
        result.error = "invalid project sample settings"; return result;
    }
    if (!project.musicalTime().valid()) { result.error = "invalid project musical timing"; return result; }
    for (const auto& source : project.sources()) if (source && (source->sampleRate < 8000 || source->sampleRate > 192000)) {
        result.error = "invalid source sample rate"; return result;
    }
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->sampleRate != project.sampleRate()) {
            result.error = "a live clip does not match the project sample rate"; return result;
        }
    }

    if (const auto error = session::validateElastic(project); !error.empty()) { result.error = error; return result; }
    if (const auto error = session::validateClipEnvelopes(project); !error.empty()) { result.error = error; return result; }
    if (const auto error = project.validateClipGroups(); !error.empty()) { result.error = error; return result; }
    int formatVersion = anyInserts(project) ? kProjectFormatVersionInserts
                                            : kProjectFormatVersionPlain;
    for (const session::Track& track : project.tracks()) {
        for (const session::Clip& clip : track.clips) {
            if (!clip.hasValidFades()) {
                result.error = "a clip has invalid fade metadata";
                return result;
            }
            if (clip.hasPartialFades())
                formatVersion = kProjectFormatVersionPartialFades;
        }
    }

    for (const spectral::MaskShape& shape : view.mask.shapes) {
        if (!shape.comb) continue;
        if (!shape.comb->validPitch()) {
            result.error = "invalid harmonic pitch contour";
            return result;
        }
        if (!shape.comb->pitch.empty())
            formatVersion = kProjectFormatVersionPitch;
    }

    bool hasAutomation = false, preciseParameters = false, automationSegments = false, smoothAutomation = false;
    bool validAutomation = project.automationPointCount() <= session::kMaxProjectAutomationPoints;
    session::PluginStateBudget stateBudget;
    std::map<std::array<std::uint8_t,16>, session::PluginState> states;
    const auto precise = [](double value) {
        return !std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()
            || double(static_cast<float>(value)) != value;
    };
    const auto checkCurve = [&](const auto& curve, double minimum, double maximum) {
        hasAutomation = hasAutomation || curve.hasData();
        validAutomation = validAutomation && curve.valid(minimum, maximum);
        for (const auto& point : curve.points) {
            preciseParameters = preciseParameters || precise(point.value);
            automationSegments = automationSegments || point.interpolation != session::AutomationInterpolation::Default;
            smoothAutomation = smoothAutomation || point.interpolation == session::AutomationInterpolation::Smooth;
        }
    };
    bool stateControls = false, instanceAutomation = false;
    const auto checkInserts = [&](const auto& inserts) {
        for (const auto& insert : inserts) {
            const bool vst3 = insert.uri.starts_with("vst3:");
            preciseParameters = preciseParameters || vst3 || !insert.instanceState.empty();
            if (insert.instanceState.size() > 2) { result.error = "an insert has too many plugin instance states"; return; }
            for (const auto& state : insert.instanceState) {
                if (state.uri() != insert.uri) { result.error = "plugin state belongs to a different plugin"; return; }
                if (!stateBudget.add(state, result.error)) return;
                states.emplace(state.id().bytes, state);
            }
            std::unordered_set<std::uint32_t> ports;
            for (const auto& control : insert.controls) {
                if (control.fromState && insert.instanceState.empty()) {
                    result.error = "state-authoritative control has no plugin state reference"; return;
                }
                stateControls = stateControls || control.fromState;
                const double limit = insert.uri.starts_with("clap:") ? std::numeric_limits<double>::max() : std::numeric_limits<float>::max();
                const double minimum = vst3 ? 0 : -limit;
                const double maximum = vst3 ? 1 : limit;
                checkCurve(control.automation, minimum, maximum);
                for (const auto& instance : control.instanceAutomation) if (instance) {
                    instanceAutomation = true;
                    checkCurve(instance->curve, minimum, maximum);
                    if (!std::isfinite(instance->base) || instance->base < minimum || instance->base > maximum) validAutomation = false;
                }
                if (!std::isfinite(control.value) || control.value < minimum || control.value > maximum
                    || (vst3 && control.port > 0x7FFFFFFF) || !ports.insert(control.port).second) validAutomation = false;
                preciseParameters = preciseParameters || precise(control.value);
            }
        }
    };
    checkCurve(project.masterVolumeAutomation(), 0, 4);
    session::forEachEffectRack(project, [&](session::EffectOwner, const auto& inserts) {
        if (result.error.empty()) checkInserts(inserts);
    });
    for (const auto& track : project.tracks()) {
        checkCurve(track.volumeAutomation, 0, 4); checkCurve(track.panAutomation, -1, 1);
        for (const auto& clip : track.clips) {
            checkCurve(clip.volumeEnvelope, 0, 4); checkCurve(clip.panEnvelope, -1, 1);
        }
    }
    for (const auto& source : project.sources()) if (source && source->crossfade)
        for (const auto* clip : {&source->crossfade->left, &source->crossfade->right}) {
            checkCurve(clip->volumeEnvelope, 0, 4); checkCurve(clip->panEnvelope, -1, 1);
        }
    if (!result.error.empty()) return result;
    if (!validAutomation) { result.error = "invalid or oversized automation curves"; return result; }
    if (hasAutomation) formatVersion = kProjectFormatVersionAutomation;

    for (const auto& source : project.sources()) {
        if (!source || !source->crossfade) continue;
        const auto& recipe = *source->crossfade;
        if (recipe.left.source >= source->id || recipe.right.source >= source->id
            || !session::validCrossfadeRecipe(project, recipe, source->audio.frames(), source->audio.channels())) {
            result.error = "invalid crossfade source recipe"; return result;
        }
        formatVersion = kProjectFormatVersionCrossfade;
    }

    const auto routing = session::compileRouting(project);
    if (!routing.ok) { result.error = routing.error; return result; }
    if (routing.active) formatVersion = kProjectFormatVersionRouting;
    bool anyMetadata = !project.metadata().empty();
    for (const auto& shared : project.sources())
        if (shared && !shared->provenance.empty()) anyMetadata = true;
    if (!view.noisePrint.empty() && view.noisePrint.channelScope != "all")
        formatVersion = kProjectFormatVersionScopedPrint;
    if (project.defaultExportBits() != 24 || std::any_of(project.sources().begin(), project.sources().end(),
        [&](const auto& source) { return source && source->sampleRate != project.sampleRate(); }))
        formatVersion = kProjectFormatVersionSourceRate;

    const auto checkElastic = [&](const session::Clip& clip) {
        if (!clip.elastic) return true;
        formatVersion = kProjectFormatVersionElastic;
        const auto* source = project.source(clip.source);
        return source && source->audio.channels() >= 1 && source->audio.channels() <= 2
            && clip.validSource(session::FramePos(source->audio.frames()));
    };
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips)
        if (!checkElastic(clip)) { result.error = "invalid elastic clip metadata"; return result; }
    for (const auto& source : project.sources()) if (source && source->crossfade)
        if (!checkElastic(source->crossfade->left) || !checkElastic(source->crossfade->right)) {
            result.error = "invalid elastic crossfade metadata"; return result;
        }

    const fs::path root(path);
    const fs::path where = layerDir(root, layer);
    std::error_code ec;
    if (auto error = storage::makeDirectories(root); !error.empty()) {
        result.error = error;
        return result;
    }
    const storage::BundleLock lock(root, true);
    if (!lock.error().empty()) { result.error = lock.error(); return result; }
    for (const fs::path& sub : {root, root / "audio", root / "peaks", root / "state", where}) {
        if (auto error = storage::makeDirectories(sub); !error.empty()) {
            result.error = error;
            return result;
        }
    }
    if (auto error = finishPendingSave(where); !error.empty()) {
        result.error = error;
        return result;
    }

    // A save that died mid-write left its temporary audio behind, and nothing
    // else ever looks for those names. The exclusive lock means none is in use.
    {
        std::error_code scan;
        for (fs::directory_iterator it(root / "audio", scan), end; !scan && it != end; it.increment(scan)) {
            const auto name = it->path().filename().string();
            const auto mark = name.rfind(".tmp-");
            if (mark != std::string::npos && name.size() - mark == 11) { std::error_code gone; fs::remove(it->path(), gone); }
        }
    }

    // --- audio, written once ------------------------------------------------------
    //
    // Only sources whose file is not already present. This is what makes the second
    // and every later save cost a few kilobytes instead of the whole session.
    std::size_t savedSources = 0;
    const auto sourceCount = std::count_if(project.sources().begin(), project.sources().end(), [](const auto& source) { return bool(source); });
    for (const auto& shared : project.sources()) {
        if (!shared) continue;
        if (cancelled()) return result;
        const session::AudioSource& source = *shared;
        if (source.uuid.isNull()) {
            result.error = "a source has no uuid; refusing to write a project that "
                           "could not be read back";
            return result;
        }

        const fs::path audioPath = root / "audio" / (source.uuid.toString() + ".wav");
        const auto existing = probeAudioFile(audioPath.string());
        const bool complete = existing.ok && existing.frames == static_cast<std::int64_t>(source.audio.frames())
                           && existing.channels == source.audio.channels()
                           && existing.sampleRate == source.sampleRate;
        const auto error = complete ? storage::syncPath(audioPath)
                                   : storage::writeAudio(audioPath, source.audio, source.sampleRate, options.cancel,
                                       [&](float fraction) { if (options.progress) options.progress("audio",
                                           static_cast<float>((double(savedSources) + fraction) / double(sourceCount))); });
        if (!error.empty()) {
            result.error = error;
            (void)cancelled();
            return result;
        }
        if (cancelled()) return result;
        ++savedSources;
        if (options.progress) options.progress("audio", static_cast<float>(double(savedSources) / double(sourceCount)));

        const fs::path peakPath = root / "peaks" / (source.uuid.toString() + ".peak");
        if (options.buildPeakCaches && !fs::exists(peakPath, ec)) {
            // A cache, so a failure here is not a failure to save.
            const PeakLevel level =
                buildPeaks(source.audio, kCachedPeakSamplesPerPeak);
            if (!level.empty())
                (void)writePeakCache(peakPath.string(), level);
        }
    }

    if (cancelled()) return result;
    if (!project.video().bundlePath.empty()) {
        if (project.video().empty()) { result.error = "a bundled video needs its source path"; return result; }
        const auto target = portableVideoPath(root, project.video().bundlePath, result.error);
        if (!result.error.empty()) return result;
        if (auto error = storage::makeDirectories(root / "media"); !error.empty()) { result.error = error; return result; }
        // A missing collected picture stays relinkable through ordinary saves.
        // Collection assigns a fresh target and therefore always requires its input.
        std::error_code pathError;
        const auto source = fs::weakly_canonical(project.video().path, pathError);
        if (pathError || source != fs::path(target)) {
            result.error = storage::copyReference(project.video().path, target, options.cancel,
                [&](float fraction) { if (options.progress) options.progress("video", fraction); });
            if (!result.error.empty()) { (void)cancelled(); return result; }
        } else if (fs::exists(target)) {
            result.error = storage::syncPath(target);
            if (!result.error.empty()) return result;
        }
    }
    if (cancelled()) return result;

    // Publish immutable state before either metadata file or the journal can
    // refer to it. An interrupted save leaves previous layers' snapshots intact.
    std::size_t stateIndex = 0;
    for (const auto& [id, state] : states) {
        (void)id;
        result.error = writePluginState(root, state, options.cancel, [&](float fraction) {
            if (options.progress) options.progress("plugin-state", float((stateIndex + double(fraction)) / states.size()));
        });
        if (!result.error.empty()) { (void)cancelled(); return result; }
        ++stateIndex;
    }
    if (cancelled()) return result;

    // --- manifest -----------------------------------------------------------------
    toml::table manifest;
    // Amendment A11: the number says what an older build would lose.
    if (anyMetadata) formatVersion = std::max(formatVersion, kProjectFormatVersionMetadata);
    if (!project.video().empty()) formatVersion = std::max(formatVersion, kProjectFormatVersionVideo);
    const auto hasClapIds = [](const auto& inserts) {
        return std::any_of(inserts.begin(), inserts.end(), [](const auto& insert) {
            return insert.uri.starts_with("clap:") && !insert.legacyClapControls;
        });
    };
    bool stableClapIds = false;
    session::forEachEffectRack(project, [&](session::EffectOwner, const auto& inserts) {
        stableClapIds = stableClapIds || hasClapIds(inserts);
    });
    if (stableClapIds) formatVersion = kProjectFormatVersionClapIds;
    if (project.musicalTime() != session::MusicalTime{})
        formatVersion = std::max(formatVersion, kProjectFormatVersionMusicalTime);
    if (!project.video().bundlePath.empty())
        formatVersion = std::max(formatVersion, kProjectFormatVersionPortableVideo);
    if (preciseParameters) formatVersion = std::max(formatVersion, kProjectFormatVersionPluginState);
    if (stateControls) formatVersion = std::max(formatVersion, kProjectFormatVersionStateControls);
    if (automationSegments) formatVersion = std::max(formatVersion, kProjectFormatVersionAutomationSegments);
    if (instanceAutomation) formatVersion = std::max(formatVersion, kProjectFormatVersionInstanceAutomation);
    if (!project.clipGroups().empty()) formatVersion = std::max(formatVersion, kProjectFormatVersionClipGroups);
    bool fadeShapes = false;
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips)
        fadeShapes = fadeShapes || shapedFades(clip);
    for (const auto& source : project.sources()) if (source && source->crossfade)
        fadeShapes = fadeShapes || shapedFades(source->crossfade->left) || shapedFades(source->crossfade->right);
    if (fadeShapes) formatVersion = std::max(formatVersion, kProjectFormatVersionFadeShapes);
    bool clipEnvelopes = false;
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips)
        clipEnvelopes = clipEnvelopes || clip.hasEnvelopes();
    for (const auto& source : project.sources()) if (source && source->crossfade)
        clipEnvelopes = clipEnvelopes || source->crossfade->left.hasEnvelopes() || source->crossfade->right.hasEnvelopes();
    if (clipEnvelopes) formatVersion = std::max(formatVersion,kProjectFormatVersionClipEnvelopes);
    if (smoothAutomation) formatVersion = std::max(formatVersion,kProjectFormatVersionSmoothAutomation);
    if (clipEffects) formatVersion = std::max(formatVersion,kProjectFormatVersionClipEffects);
    if (std::any_of(project.tracks().begin(), project.tracks().end(), [](const auto& t) { return t.ducking.has_value(); }))
        formatVersion = std::max(formatVersion,kProjectFormatVersionDuckingRecipe);
    manifest.insert("formatVersion", static_cast<std::int64_t>(formatVersion));
    manifest.insert("sampleRate", static_cast<std::int64_t>(project.sampleRate()));
    manifest.insert("defaultExportBits", static_cast<std::int64_t>(project.defaultExportBits()));
    manifest.insert("generator", "omaaudio");
    manifest.insert("modified", nowIso8601());

    // Preserve the original creation time across saves rather than restamping it.
    // A recovery write takes the project's own stamp when there is one.
    const fs::path manifestPath = where / "manifest.toml";
    std::string created = nowIso8601();
    for (const fs::path& candidate : {root / "manifest.toml", manifestPath}) {
        if (!fs::exists(candidate, ec))
            continue;
        const ParsedToml existing = parseTomlFile(candidate);
        if (existing.ok) {
            if (const auto value = existing.table["created"].value<std::string>()) {
                created = *value;
                break;
            }
        }
    }
    manifest.insert("created", created);

    // --- session ------------------------------------------------------------------
    toml::table session;

    toml::array sources;
    for (const auto& shared : project.sources()) {
        if (!shared) continue;
        const session::AudioSource& source = *shared;
        toml::table entry;
        entry.insert("uuid", source.uuid.toString());
        entry.insert("name", source.name);
        entry.insert("originalPath", source.originalPath);
        entry.insert("frames", static_cast<std::int64_t>(source.audio.frames()));
        entry.insert("channels", static_cast<std::int64_t>(source.audio.channels()));
        entry.insert("sampleRate", static_cast<std::int64_t>(source.sampleRate));
        if (source.crossfade) {
            const auto& r = *source.crossfade;
            toml::table recipe;
            recipe.insert("start", static_cast<std::int64_t>(r.start));
            recipe.insert("end", static_cast<std::int64_t>(r.end));
            recipe.insert("equalPower", r.equalPower);
            // Source recipes describe audio, not live editing membership.
            recipe.insert("clip", clipsToToml({r.left, r.right}, project, false));
            entry.insert("crossfade", std::move(recipe));
        }
        if (source.originalSampleRate.has_value()) {
            entry.insert("originalSampleRate",
                         static_cast<std::int64_t>(*source.originalSampleRate));
        }
        if (!source.provenance.empty()) entry.insert("provenance", provenanceToToml(source.provenance));
        sources.push_back(std::move(entry));
    }
    session.insert("source", std::move(sources));
    if (!project.clipGroups().empty()) {
        toml::array groups;
        for (const auto& group : project.clipGroups())
            groups.push_back(toml::table{{"id", std::int64_t(group.id)}, {"name", group.name}, {"color", group.colour}});
        session.insert("clipGroup", std::move(groups));
    }
    if (project.musicalTime() != session::MusicalTime{}) {
        const auto& music = project.musicalTime();
        session.insert("musicalTime", toml::table{{"enabled", music.enabled},
            {"tempoMilli", music.tempoMilli}, {"beats", music.beats}, {"unit", music.unit}});
    }
    if (!project.metadata().empty()) {
        const session::ProjectMetadata& pm = project.metadata();
        toml::table metadata;
        metadata.insert("description", pm.description);
        metadata.insert("originator", pm.originator);
        metadata.insert("originatorReference", pm.originatorReference);
        metadata.insert("originationDate", pm.originationDate);
        metadata.insert("originationTime", pm.originationTime);
        metadata.insert("timeReferenceSet", pm.timeReferenceSet);
        metadata.insert("timeReference", static_cast<std::int64_t>(pm.timeReference));
        metadata.insert("codingHistory", pm.codingHistory);
        session.insert("metadata", std::move(metadata));
    }
    if (!project.video().empty()) {
        toml::table video;
        video.insert("path", project.video().bundlePath.empty() ? project.video().path : project.video().bundlePath);
        if (!project.video().bundlePath.empty()) video.insert("pathBase", "bundle");
        video.insert("offset", static_cast<std::int64_t>(project.video().offset));
        video.insert("dropFrame", project.video().dropFrame);
        session.insert("video", std::move(video));
    }

    toml::array tracks;
    ClipRackIds clipRacks;
    for (const session::Track& track : project.tracks()) {
        toml::table entry;
        entry.insert("id", trackWire(track.id));
        entry.insert("name", track.name);
        entry.insert("role", std::string(session::roleName(track.role)));
        if (!track.colour.empty())
            entry.insert("color", track.colour);
        entry.insert("height", std::string(session::heightName(track.height)));
        entry.insert("gain", static_cast<double>(track.gain));
        entry.insert("pan", static_cast<double>(track.pan));
        if (track.volumeAutomation.hasData()) entry.insert("volumeAutomation", automationToToml(track.volumeAutomation));
        if (track.panAutomation.hasData()) entry.insert("panAutomation", automationToToml(track.panAutomation));
        entry.insert("muted", track.muted);
        entry.insert("soloed", track.soloed);
        entry.insert("clip", clipsToToml(track.clips, project, true, &clipRacks));
        if (!track.inserts.empty())
            entry.insert("insert", insertsToToml(track.inserts));
        if (!track.routing.direct()) entry.insert("routing", routingToToml(track.routing));
        if (const auto& recipe = track.ducking) {
            toml::array triggers;
            for (const auto trigger : recipe->triggers) triggers.push_back(trackWire(trigger));
            entry.insert("ducking", toml::table{{"triggers", std::move(triggers)},
                {"start", static_cast<std::int64_t>(recipe->start)}, {"end", static_cast<std::int64_t>(recipe->end)},
                {"thresholdDb", recipe->thresholdDb}, {"reductionDb", recipe->reductionDb},
                {"attackSeconds", recipe->attackSeconds}, {"holdSeconds", recipe->holdSeconds},
                {"releaseSeconds", recipe->releaseSeconds}, {"replaceInRange", recipe->replaceInRange},
                {"baseline", automationToToml(recipe->baseline)}, {"generated", automationToToml(recipe->generated)}});
        }
        tracks.push_back(std::move(entry));
    }
    session.insert("track", std::move(tracks));
    if (!clipRacks.empty()) {
        toml::array racks;
        for (const auto* rack : clipRacks) {
            const auto* source = project.source(rack->source);
            if (!source) { result.error = "a clip effect region refers to a missing source"; return result; }
            toml::table entry{{"source", source->uuid.toString()},
                {"sourceOffset", static_cast<std::int64_t>(rack->sourceOffset)},
                {"length", static_cast<std::int64_t>(rack->length)}};
            if (rack->elastic) {
                const auto& e = *rack->elastic;
                entry.insert("elastic", toml::table{{"processor","signalsmith-v1"},
                    {"inputStart",e.inputStart},{"inputFrames",e.inputFrames},
                    {"outputFrames",e.outputFrames},{"semitones",double(e.semitones)}});
            }
            entry.insert("insert", insertsToToml(rack->inserts));
            racks.push_back(std::move(entry));
        }
        session.insert("clipRack", std::move(racks));
    }

    toml::array markers;
    for (const session::Marker& m : project.markers()) {
        toml::table entry;
        entry.insert("id", markerWire(m.id));
        entry.insert("name", m.name);
        entry.insert("position", static_cast<std::int64_t>(m.position));
        entry.insert("length", static_cast<std::int64_t>(m.length));
        entry.insert("type", std::string(session::markerTypeName(m.type)));
        if (!m.colour.empty())
            entry.insert("color", m.colour);
        if (!m.notes.empty())
            entry.insert("notes", m.notes);
        markers.push_back(std::move(entry));
    }
    session.insert("marker", std::move(markers));

    // The master bus: additive keys, so a file without them is a bus at unity with
    // no inserts. An insert on it moves the format number like a track's does.
    toml::table master;
    master.insert("trim", static_cast<double>(project.masterTrim()));
    if (project.masterVolumeAutomation().hasData()) master.insert("volumeAutomation", automationToToml(project.masterVolumeAutomation()));
    if (!project.masterInserts().empty())
        master.insert("insert", insertsToToml(project.masterInserts()));
    session.insert("master", std::move(master));

    toml::table viewTable;
    viewTable.insert("focus", trackWire(view.focus));
    viewTable.insert("selectionStart", static_cast<std::int64_t>(view.selection.start));
    viewTable.insert("selectionEnd", static_cast<std::int64_t>(view.selection.end));
    viewTable.insert("selectionTrack", trackWire(view.selection.track));
    viewTable.insert("selectionChannel", view.selection.channel);
    session.insert("view", std::move(viewTable));

    // The Spectral page's state (A22): additive, so an older reader skips it.
    session.insert("spectral", spectralToToml(view));
    toml::table sound;
    for (const auto& [track, choices] : view.sound) {
        if (project.track(track) == nullptr || choices.empty()) continue;
        toml::table entry;
        for (const auto& [key, value] : choices) if (std::isfinite(value)) entry.insert(key, value);
        sound.insert(std::to_string(trackWire(track)), std::move(entry));
    }
    if (!sound.empty()) session.insert("sound", std::move(sound));

    // All audio is durable before the journal can refer to it, including directory
    // entries from an earlier failed save that left complete WAVs behind.
    if (auto error = storage::syncPath(root / "audio", true); !error.empty()) {
        result.error = error; return result;
    }
    toml::table journal;
    journal.insert("manifest", std::move(manifest));
    journal.insert("session", std::move(session));
    const auto text = tomlText(journal);
    // The journal outranks the files beneath it on every later load and save,
    // so one that cannot be read back must never be published.
    try { (void)toml::parse(text); }
    catch (const std::exception&) {
        result.error = "the project could not be written as text that reads back; nothing was changed on disk";
        return result;
    }
    if (options.progress) options.progress("metadata", 0);
    if (cancelled()) return result;
    // Once published, the journal must finish as one transaction; cancellation
    // cannot interrupt this small final metadata commit.
    if (auto error = storage::writeText(where / kPendingSave, text); !error.empty()) {
        result.error = error; return result;
    }
    if (auto error = finishPendingSave(where); !error.empty()) {
        result.error = error; return result;
    }

    result.ok = true;
    if (options.progress) options.progress("metadata", 1);
    return result;
}

namespace {
class CleanupAudioDirectory {
public:
    explicit CleanupAudioDirectory(const fs::path& root) {
        if (fs::is_symlink(fs::symlink_status(root))) throw std::runtime_error("cleanup refuses a symlinked project directory");
        root_ = root;
        if (::lstat(root.c_str(), &rootInfo_) != 0) throw std::system_error(errno, std::generic_category(), "inspect project directory");
        fd_ = ::open((root / "audio").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open project audio for cleanup");
    }
    ~CleanupAudioDirectory() { if (fd_ >= 0) ::close(fd_); }
    CleanupAudioDirectory(const CleanupAudioDirectory&) = delete;
    CleanupAudioDirectory& operator=(const CleanupAudioDirectory&) = delete;
    int fd() const { return fd_; }
    bool unchanged() const {
        struct stat root{}, path{}, pinned{};
        return ::lstat(root_.c_str(), &root) == 0 && root.st_dev == rootInfo_.st_dev && root.st_ino == rootInfo_.st_ino
            && ::lstat((root_ / "audio").c_str(), &path) == 0 && S_ISDIR(path.st_mode)
            && ::fstat(fd_, &pinned) == 0 && path.st_dev == pinned.st_dev && path.st_ino == pinned.st_ino;
    }
private:
    int fd_ = -1;
    fs::path root_;
    struct stat rootInfo_{};
};

std::unordered_set<std::string> cleanupProtected(const fs::path& root,
    const std::vector<std::string>& protectedUuids)
{
    std::unordered_set<std::string> protectedNames;
    const auto protect = [&](const std::string& value) {
        const auto uuid = session::Uuid::parse(value);
        if (!uuid || uuid->isNull()) throw std::runtime_error("cleanup found a malformed protected audio id");
        protectedNames.insert(uuid->toString() + ".wav");
    };
    for (const auto& uuid : protectedUuids) protect(uuid);
    for (const auto& where : {root, root / "recovery"}) {
        if (fs::is_symlink(fs::symlink_status(where))) throw std::runtime_error("cleanup refuses symlinked metadata");
        for (const auto* name : {"manifest.toml", "session.toml", kPendingSave})
            if (fs::is_symlink(fs::symlink_status(where / name))) throw std::runtime_error("cleanup refuses symlinked metadata files");
        if (where != root && !fs::exists(where / "manifest.toml") && !fs::exists(where / "session.toml")
            && !fs::exists(where / kPendingSave)) continue;
        if (const auto error = finishPendingSave(where); !error.empty()) throw std::runtime_error(error);
        const auto data = readMetadata(where);
        if (!data.ok) throw std::runtime_error(data.error);
        const auto version = data.manifest["formatVersion"].value<std::int64_t>();
        if (!version || *version < 1 || *version > kProjectFormatVersion
            || !data.session["source"].is_array() || !data.session["track"].is_array())
            throw std::runtime_error("cleanup needs valid supported project and recovery metadata");
        // Preserve source declarations AND references, including recipe inputs.
        // A dangling clip reference must never turn its audio into a candidate.
        std::function<void(const toml::node&)> visit = [&](const toml::node& node) {
            if (const auto* table = node.as_table()) {
                for (const auto& [key, value] : *table) {
                    if (key.str() == "uuid" || (key.str() == "source" && value.is_string())) {
                        const auto text = value.value<std::string>();
                        if (!text) throw std::runtime_error("cleanup found a malformed source declaration");
                        protect(*text);
                    } else visit(value);
                }
            } else if (const auto* array = node.as_array()) for (const auto& value : *array) visit(value);
        };
        visit(data.session);
        for (const auto& source : *data.session["source"].as_array())
            if (!source.is_table() || !source.as_table()->at("uuid").is_string())
                throw std::runtime_error("cleanup found an incomplete source declaration");
    }
    return protectedNames;
}

std::optional<UnusedAudioFile> cleanupIdentity(int fd, const std::string& name)
{
    if (name.size() != 40 || name.substr(36) != ".wav") return {};
    const auto uuid = session::Uuid::parse(name.substr(0,36));
    if (!uuid || uuid->isNull() || uuid->toString() + ".wav" != name) return {};
    struct stat info{};
    if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "inspect cleanup audio");
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) return {};
    return UnusedAudioFile{name, std::uint64_t(info.st_size), std::uint64_t(info.st_dev), std::uint64_t(info.st_ino),
        std::int64_t(info.st_mtim.tv_sec), std::int64_t(info.st_mtim.tv_nsec)};
}

std::vector<UnusedAudioFile> unusedAudioFiles(int fd, const std::unordered_set<std::string>& protectedNames)
{
    std::vector<UnusedAudioFile> files;
    // Traverse the pinned directory descriptor, so renaming it cannot redirect an
    // apply operation to a different audio folder after inspection.
    for (const auto& entry : fs::directory_iterator("/proc/self/fd/" + std::to_string(fd))) {
        const auto name = entry.path().filename().string();
        if (protectedNames.contains(name)) continue;
        if (auto file = cleanupIdentity(fd, name)) files.push_back(std::move(*file));
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return files;
}
}

AudioCleanupPreview inspectUnusedAudio(const std::string& path, const std::vector<std::string>& protectedUuids)
{
    AudioCleanupPreview out;
    try {
        const fs::path root(path);
        const storage::BundleLock lock(root, true);
        if (!lock.error().empty()) { out.error = lock.error(); return out; }
        const CleanupAudioDirectory directory(root);
        out.files = unusedAudioFiles(directory.fd(), cleanupProtected(root, protectedUuids));
        if (!directory.unchanged()) throw std::runtime_error("project directories changed during inspection");
        out.ok = true;
    } catch (const std::exception& e) { out.error = e.what(); }
    return out;
}

AudioCleanupResult cleanupUnusedAudio(const std::string& path, const std::vector<UnusedAudioFile>& preview,
    const std::vector<std::string>& protectedUuids, const std::atomic<bool>* cancel)
{
    AudioCleanupResult out;
    try {
        const fs::path root(path);
        const storage::BundleLock lock(root, true);
        if (!lock.error().empty()) { out.error = lock.error(); return out; }
        const CleanupAudioDirectory directory(root);
        const auto protectedNames = cleanupProtected(root, protectedUuids);
        // Validate every candidate before removing any. New references or changed
        // files make the preview stale; a later preview can explain the new set.
        std::unordered_set<std::string> unique;
        for (const auto& file : preview) {
            const auto current = cleanupIdentity(directory.fd(), file.name);
            if (!unique.insert(file.name).second || protectedNames.contains(file.name) || !current || *current != file) {
                out.error = "cleanup preview changed; inspect unused audio again"; return out;
            }
        }
        for (const auto& file : preview) {
            if (cancel && cancel->load(std::memory_order_relaxed)) { out.cancelled = true; break; }
            if (!directory.unchanged()) { out.error = "project directories changed during cleanup"; break; }
            const auto current = cleanupIdentity(directory.fd(), file.name);
            if (!current || *current != file) { out.error = "cleanup audio changed during removal"; break; }
            if (::unlinkat(directory.fd(), file.name.c_str(), 0) != 0) {
                out.error = std::system_error(errno, std::generic_category(), "remove unused audio").what(); break;
            }
            ++out.removed; out.bytes += file.bytes;
        }
        if (out.removed) {
            int synced;
            do { synced = ::fsync(directory.fd()); } while (synced != 0 && errno == EINTR);
            if (synced != 0 && out.error.empty()) out.error = "could not sync the audio directory after cleanup";
        }
        out.ok = out.error.empty() && !out.cancelled;
    } catch (const std::exception& e) { out.error = e.what(); }
    return out;
}

LoadResult loadProject(const std::string& path, Layer layer)
{
    LoadResult result;

    const fs::path root(path);
    StateLoader states(root);
    const fs::path where = layerDir(root, layer);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        result.error = path + " is not an Omawave project directory";
        return result;
    }
    const storage::BundleLock lock(root, false);
    if (!lock.error().empty()) { result.error = lock.error(); return result; }
    const auto metadata = readMetadata(where);
    if (!metadata.ok) { result.error = metadata.error; return result; }
    const auto& manifest = metadata.manifest;

    const auto version = manifest["formatVersion"].value<std::int64_t>();
    if (!version.has_value()) {
        result.error = "manifest.toml has no formatVersion";
        return result;
    }
    if (*version > kProjectFormatVersion) {
        // Refused outright rather than read as far as it parses. Half-reading a newer
        // project and then saving over it is how a project gets destroyed by an older
        // build of the thing that made it.
        result.error = "this project was written by a newer version of Omawave "
                       "(format " + std::to_string(*version) + ", this build reads "
                     + std::to_string(kProjectFormatVersion) + ")";
        return result;
    }

    const auto rate = manifest["sampleRate"].value<std::int64_t>();
    if (!rate.has_value() || *rate < 8000 || *rate > 192000) {
        result.error = "manifest.toml has no usable sampleRate";
        return result;
    }

    const auto& session = metadata.session;

    session::Project project(static_cast<std::uint32_t>(*rate));
    const auto bitsNode = manifest["defaultExportBits"];
    const auto bits = bitsNode.value<std::int64_t>().value_or(24);
    if ((bitsNode && !bitsNode.is_integer()) || (bits != 16 && bits != 24 && bits != 32)
        || (*version < kProjectFormatVersionSourceRate && bits != 24)) {
        result.error = "invalid default export sample type"; return result;
    }
    project.setDefaultExportBits(std::uint32_t(bits));
    if (const auto groups = session["clipGroup"]; groups) {
        if (*version < kProjectFormatVersionClipGroups || !groups.is_array()) {
            result.error = "clip groups require project format 20 and a group list"; return result;
        }
        for (const auto& node : *groups.as_array()) {
            const auto* entry = node.as_table();
            if (!entry) { result.error = "invalid clip group entry"; return result; }
            const auto id = (*entry)["id"].value<std::int32_t>();
            const auto name = (*entry)["name"].value<std::string>();
            const auto colour = (*entry)["color"].value<std::string>();
            if (!(*entry)["id"].is_integer() || !id || *id < 0 || !name || !colour
                || !project.insertClipGroup({session::ClipGroupId(*id), *name, *colour})) {
                result.error = "invalid or duplicate clip group"; return result;
            }
        }
    }
    if (const auto music = session["musicalTime"]; music) {
        const auto tempo = music["tempoMilli"].value<std::int32_t>();
        const auto beats = music["beats"].value<std::int32_t>();
        const auto unit = music["unit"].value<std::int32_t>();
        const auto enabled = music["enabled"].value<bool>();
        if (*version < kProjectFormatVersionMusicalTime || !music.is_table()
            || !music["tempoMilli"].is_integer() || !music["beats"].is_integer() || !music["unit"].is_integer()
            || !music["enabled"].is_boolean() || !tempo || !beats || !unit || !enabled) {
            result.error = "invalid project musical timing"; return result;
        }
        project.musicalTime() = {*enabled, *tempo, *beats, *unit};
        if (!project.musicalTime().valid()) { result.error = "invalid project musical timing"; return result; }
    }


    // --- sources ------------------------------------------------------------------
    if (const toml::array* sources = session["source"].as_array()) {
        for (const toml::node& node : *sources) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;

            const auto uuidText = (*entry)["uuid"].value<std::string>();
            if (!uuidText.has_value()) {
                result.error = "a source in session.toml has no uuid";
                return result;
            }
            const std::optional<session::Uuid> uuid = session::Uuid::parse(*uuidText);
            if (!uuid.has_value()) {
                result.error = "a source in session.toml has a malformed uuid: "
                             + *uuidText;
                return result;
            }

            const fs::path audioPath = root / "audio" / (*uuidText + ".wav");
            AudioFile file = readAudioFile(audioPath.string());
            const auto frames = (*entry)["frames"].value<std::int64_t>();
            const auto channels = (*entry)["channels"].value<std::int64_t>();
            const auto sourceRateNode = (*entry)["sampleRate"];
            const auto sourceRate = sourceRateNode.value<std::int64_t>().value_or(*rate);
            if ((sourceRateNode && !sourceRateNode.is_integer()) || sourceRate < 8000 || sourceRate > 192000
                || (*version < kProjectFormatVersionSourceRate && sourceRate != *rate)) {
                result.error = "invalid stored source sample rate"; return result;
            }
            if (!file.ok || !file.error.empty() || !frames || !channels
                || *frames != static_cast<std::int64_t>(file.audio.frames())
                || *channels != static_cast<std::int64_t>(file.channels)
                || file.sampleRate != static_cast<std::uint32_t>(sourceRate)) {
                result.error = "could not read " + audioPath.string() + ": " + file.error;
                if (file.error.empty()) result.error += "audio does not match the saved source";
                return result;
            }

            std::optional<std::uint32_t> originalRate;
            if (const auto value = (*entry)["originalSampleRate"].value<std::int64_t>())
                originalRate = static_cast<std::uint32_t>(*value);

            std::string name = (*entry)["name"].value_or(std::string{});
            if (name.empty())
                name = *uuidText;

            if (project.sourceByUuid(*uuid)) { result.error = "duplicate source uuid"; return result; }
            std::optional<session::CrossfadeRecipe> crossfade;
            if ((*entry)["crossfade"]) {
                crossfade = crossfadeFromToml(*entry, project, file.audio.frames(), file.channels, *version);
                if (*version < kProjectFormatVersionCrossfade || !crossfade) {
                    result.error = "invalid crossfade source recipe"; return result;
                }
            }
            session::AudioMetadata provenance;
            if (const toml::table* table = (*entry)["provenance"].as_table()) {
                if (*version < kProjectFormatVersionMetadata) { result.error = "source provenance needs project format 11"; return result; }
                if (!provenanceFromToml(*table, provenance)) { result.error = "invalid source provenance"; return result; }
            }
            (void)project.addSource(std::move(name),
                                    (*entry)["originalPath"].value_or(std::string{}),
                                    std::move(file.audio), originalRate, *uuid, std::move(crossfade), std::uint32_t(sourceRate),
                                    std::move(provenance));

            PeakLevel cached =
                readPeakCache((root / "peaks" / (*uuidText + ".peak")).string());
            if (!cached.empty())
                result.peaks.emplace(*uuidText, std::move(cached));
        }
    }

    // --- the project's own metadata (format 11) -----------------------------------
    if (const toml::table* metadata = session["metadata"].as_table()) {
        if (*version < kProjectFormatVersionMetadata) { result.error = "project metadata needs project format 11"; return result; }
        session::ProjectMetadata& pm = project.metadata();
        pm.description = (*metadata)["description"].value_or(std::string{});
        pm.originator = (*metadata)["originator"].value_or(std::string{});
        pm.originatorReference = (*metadata)["originatorReference"].value_or(std::string{});
        pm.originationDate = (*metadata)["originationDate"].value_or(std::string{});
        pm.originationTime = (*metadata)["originationTime"].value_or(std::string{});
        pm.timeReferenceSet = (*metadata)["timeReferenceSet"].value_or(false);
        pm.timeReference = (*metadata)["timeReference"].value_or(std::int64_t{0});
        pm.codingHistory = (*metadata)["codingHistory"].value_or(std::string{});
        if (pm.timeReference < 0) { result.error = "invalid project time reference"; return result; }
    }

    // --- the reference picture (format 12) ----------------------------------------
    if (const toml::table* video = session["video"].as_table()) {
        if (*version < kProjectFormatVersionVideo) { result.error = "a video reference needs project format 12"; return result; }
        session::VideoReference& v = project.video();
        v.path = (*video)["path"].value_or(std::string{});
        v.offset = (*video)["offset"].value_or(std::int64_t{0});
        v.dropFrame = (*video)["dropFrame"].value_or(false);
        if (v.path.empty()) { result.error = "a video reference needs a path"; return result; }
        if (video->contains("pathBase")) {
            if ((*video)["pathBase"].value_or(std::string{}) != "bundle" || *version < kProjectFormatVersionPortableVideo) {
                result.error = "a bundle-relative video needs project format 15 and pathBase bundle"; return result;
            }
            v.bundlePath = v.path;
            v.path = portableVideoPath(root, v.bundlePath, result.error);
            if (!result.error.empty()) return result;
        }
    }

    // --- clip effect regions (format 24) ------------------------------------------
    std::vector<std::shared_ptr<const session::ClipRack>> clipRacks;
    if (const auto* declared = session.get("clipRack")) {
        const auto* racks = declared->as_array();
        if (*version < kProjectFormatVersionClipEffects || !racks) {
            result.error = "clip effects need project format 24"; return result;
        }
        for (const toml::node& node : *racks) {
            const toml::table* entry = node.as_table();
            const auto sourceText = entry ? (*entry)["source"].value<std::string>() : std::nullopt;
            const auto uuid = sourceText ? session::Uuid::parse(*sourceText) : std::nullopt;
            const auto sourceId = uuid ? project.sourceIdByUuid(*uuid) : std::nullopt;
            const auto offset = entry ? (*entry)["sourceOffset"].value<std::int64_t>() : std::nullopt;
            const auto length = entry ? (*entry)["length"].value<std::int64_t>() : std::nullopt;
            if (!sourceId || !offset || !length) { result.error = "invalid clip effect region"; return result; }
            auto rack = std::make_shared<session::ClipRack>();
            rack->source = *sourceId; rack->sourceOffset = *offset; rack->length = *length;
            session::Clip mapping;
            if (!elasticFromToml(*entry, mapping, *version)) { result.error = "invalid clip effect stretch"; return result; }
            rack->elastic = mapping.elastic;
            rack->inserts = insertsFromToml(entry->get("insert"), *version, states, result.error);
            if (!result.error.empty()) return result;
            if (!rack->validOrigin()) { result.error = "invalid clip effect region"; return result; }
            clipRacks.push_back(std::move(rack));
        }
    }
    std::vector<bool> usedRacks(clipRacks.size(), false);

    // --- tracks -------------------------------------------------------------------
    if (const toml::array* tracks = session["track"].as_array()) {
        std::size_t index = 0;
        for (const toml::node& node : *tracks) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;

            session::Track track;
            if (*version >= kProjectFormatVersionRouting) {
                const auto id = (*entry)["id"].value<std::int64_t>();
                if (!(*entry)["id"].is_integer() || !id || *id < 0 || *id >= static_cast<std::int64_t>(session::kNoTrackOutput)) {
                    result.error = "invalid routing track id"; return result;
                }
            }
            track.id = trackFromWire((*entry)["id"].value_or<std::int64_t>(-1));
            track.name = (*entry)["name"].value_or(std::string{});
            if (const auto role = session::roleFromName(
                    (*entry)["role"].value_or(std::string{})))
                track.role = *role;
            track.colour = (*entry)["color"].value_or(std::string{});
            if (const auto height = session::heightFromName(
                    (*entry)["height"].value_or(std::string{})))
                track.height = *height;
            track.inserts = insertsFromToml(entry->get("insert"), *version, states, result.error);
            if (!result.error.empty()) return result;
            if (!automationFromToml(entry->get("volumeAutomation"), track.volumeAutomation, 0, 4, *version, result.error)
                || !automationFromToml(entry->get("panAutomation"), track.panAutomation, -1, 1, *version, result.error)) return result;
            track.gain = static_cast<float>((*entry)["gain"].value_or<double>(1.0));
            track.pan = static_cast<float>((*entry)["pan"].value_or<double>(0.0));
            track.muted = (*entry)["muted"].value_or<bool>(false);
            track.soloed = (*entry)["soloed"].value_or<bool>(false);
            if (const auto* declared = entry->get("ducking")) {
                const auto* table = declared->as_table();
                const auto* triggers = table ? (*table)["triggers"].as_array() : nullptr;
                if (*version < kProjectFormatVersionDuckingRecipe || !triggers) {
                    result.error = "a generated ducking recipe needs project format 25"; return result;
                }
                session::DuckingRecipe recipe;
                for (const auto& trigger : *triggers) {
                    const auto id = trigger.value<std::int64_t>();
                    if (!trigger.is_integer() || !id || *id < 0) { result.error = "invalid ducking trigger track"; return result; }
                    recipe.triggers.push_back(trackFromWire(*id));
                }
                const auto integer = [&](const char* key, session::FramePos& out) {
                    const auto value = (*table)[key].value<std::int64_t>(); if (!value) return false; out = *value; return true; };
                const auto number = [&](const char* key, double& out) {
                    const auto value = (*table)[key].value<double>(); if (!value || !std::isfinite(*value)) return false; out = *value; return true; };
                const auto replace = (*table)["replaceInRange"].value<bool>();
                if (!integer("start", recipe.start) || !integer("end", recipe.end)
                    || !number("thresholdDb", recipe.thresholdDb) || !number("reductionDb", recipe.reductionDb)
                    || !number("attackSeconds", recipe.attackSeconds) || !number("holdSeconds", recipe.holdSeconds)
                    || !number("releaseSeconds", recipe.releaseSeconds) || !replace) {
                    result.error = "invalid generated ducking settings"; return result;
                }
                recipe.replaceInRange = *replace;
                if (!automationFromToml(table->get("baseline"), recipe.baseline, 0, 4, *version, result.error)
                    || !automationFromToml(table->get("generated"), recipe.generated, 0, 4, *version, result.error)) return result;
                if (!recipe.valid()) { result.error = "invalid generated ducking settings"; return result; }
                track.ducking = std::move(recipe);
            }
            if (!routingFromToml(entry->get("routing"), track.routing, *version)) {
                result.error = "invalid track routing metadata"; return result;
            }

            if (const toml::array* clips = (*entry)["clip"].as_array()) {
                for (const toml::node& clipNode : *clips) {
                    const toml::table* clipEntry = clipNode.as_table();
                    if (clipEntry == nullptr)
                        continue;

                    const auto sourceText = (*clipEntry)["source"].value<std::string>();
                    if (!sourceText.has_value())
                        continue;
                    const std::optional<session::Uuid> sourceUuid =
                        session::Uuid::parse(*sourceText);
                    if (!sourceUuid.has_value())
                        continue;
                    const std::optional<session::SourceId> sourceId =
                        project.sourceIdByUuid(*sourceUuid);
                    if (!sourceId.has_value()) {
                        result.error = "a clip refers to source " + *sourceText
                                     + ", which this project does not contain";
                        return result;
                    }

                    if (project.source(*sourceId)->sampleRate != project.sampleRate()) {
                        result.error = "a live clip does not match the project sample rate"; return result;
                    }
                    session::Clip clip;
                    clip.source = *sourceId;
                    clip.timelineStart = (*clipEntry)["timelineStart"].value_or<std::int64_t>(0);
                    clip.sourceOffset = (*clipEntry)["sourceOffset"].value_or<std::int64_t>(0);
                    if (const auto group = (*clipEntry)["group"]; group) {
                        const auto id = group.value<std::int32_t>();
                        if (*version < kProjectFormatVersionClipGroups || !group.is_integer() || !id || *id < 0
                            || !project.clipGroup(session::ClipGroupId(*id))) {
                            result.error = "invalid or missing clip group"; return result;
                        }
                        clip.group = session::ClipGroupId(*id);
                    }
                    clip.length = (*clipEntry)["length"].value_or<std::int64_t>(0);
                    clip.gain = static_cast<float>((*clipEntry)["gain"].value_or<double>(1.0));
                    clip.fadeIn = (*clipEntry)["fadeIn"].value_or<std::int64_t>(0);
                    clip.fadeOut = (*clipEntry)["fadeOut"].value_or<std::int64_t>(0);
                    clip.fadeInOffset = (*clipEntry)["fadeInOffset"].value_or<std::int64_t>(0);
                    clip.fadeOutOffset = (*clipEntry)["fadeOutOffset"].value_or<std::int64_t>(0);
                    for (const char* key : {"fadeIn", "fadeOut", "fadeInOffset", "fadeOutOffset"}) {
                        const auto field = (*clipEntry)[key];
                        if (field && !field.is_integer()) {
                            result.error = std::string("a clip has invalid fade metadata: ") + key;
                            return result;
                        }
                    }
                    if (!clipEnvelopesFromToml(*clipEntry,clip,*version)) {
                        result.error = "a clip has invalid envelope metadata or needs project format 22";
                        return result;
                    }
                    if (!fadeShapesFromToml(*clipEntry, clip, *version)) {
                        result.error = "a clip has invalid fade metadata";
                        return result;
                    }
                    if (!elasticFromToml(*clipEntry,clip,*version)
                        || (clip.elastic && (!clip.validSource(session::FramePos(project.source(*sourceId)->audio.frames()))
                            || project.source(*sourceId)->audio.channels() > 2))) {
                        result.error = "invalid elastic clip metadata"; return result;
                    }
                    if (const auto rack = (*clipEntry)["rack"]; rack) {
                        const auto id = rack.value<std::int64_t>();
                        const auto offset = (*clipEntry)["effectOffset"];
                        if (!rack.is_integer() || !id || *id < 0 || std::size_t(*id) >= clipRacks.size()
                            || (offset && !offset.is_integer())) {
                            result.error = "invalid clip effect reference"; return result;
                        }
                        clip.rack = clipRacks[std::size_t(*id)];
                        clip.effectOffset = offset.value_or<std::int64_t>(0);
                        usedRacks[std::size_t(*id)] = true;
                        if (!clip.hasValidEffects()) { result.error = "a clip does not match its effect region"; return result; }
                    } else if ((*clipEntry)["effectOffset"]) {
                        result.error = "a clip effect offset has no effect region"; return result;
                    }
                    if (clip.length > 0)
                        track.clips.push_back(clip);
                }
            }

            project.insertTrack(index++, std::move(track));
        }
    }

    if (const auto routing = session::compileRouting(project); !routing.ok) {
        result.error = routing.error; return result;
    }

    if (const auto error = session::validateElastic(project); !error.empty()) { result.error = error; return result; }
    for (const auto& track : project.tracks()) if (track.ducking)
        for (const auto trigger : track.ducking->triggers)
            if (trigger == track.id || !project.track(trigger)) {
                result.error = "a generated ducking recipe refers to a missing trigger track"; return result;
            }
    if (std::find(usedRacks.begin(), usedRacks.end(), false) != usedRacks.end()) {
        result.error = "a clip effect region is not used by any clip"; return result;
    }
    if (!clipRacks.empty())
        if (const auto error = session::validateClipRacks(project); !error.empty()) { result.error = error; return result; }

    // --- markers ------------------------------------------------------------------
    if (const toml::array* markers = session["marker"].as_array()) {
        for (const toml::node& node : *markers) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;
            session::Marker m;
            const auto rawId = (*entry)["id"].value_or<std::int64_t>(-1);
            m.id = rawId < 0 ? session::kInvalidMarker
                             : static_cast<session::MarkerId>(
                                   static_cast<std::uint32_t>(rawId));
            m.name = (*entry)["name"].value_or(std::string{});
            m.position = std::max<std::int64_t>(0, (*entry)["position"].value_or<std::int64_t>(0));
            m.length = std::max<std::int64_t>(0, (*entry)["length"].value_or<std::int64_t>(0));
            if (const auto type = session::markerTypeFromName(
                    (*entry)["type"].value_or(std::string{})))
                m.type = *type;
            m.colour = (*entry)["color"].value_or(std::string{});
            m.notes = (*entry)["notes"].value_or(std::string{});
            if (m.id == session::kInvalidMarker)
                project.addMarker(std::move(m));
            else
                project.insertMarker(std::move(m));
        }
    }

    // --- master bus -------------------------------------------------------------
    if (const toml::table* master = session["master"].as_table()) {
        project.setMasterTrim(static_cast<float>((*master)["trim"].value_or<double>(1.0)));
        project.masterInserts() = insertsFromToml(master->get("insert"), *version, states, result.error);
        if (!result.error.empty()) return result;
        if (!automationFromToml(master->get("volumeAutomation"), project.masterVolumeAutomation(), 0, 4, *version, result.error)) return result;
    }

    // --- view ---------------------------------------------------------------------
    if (const toml::table* viewTable = session["view"].as_table()) {
        result.view.focus = trackFromWire((*viewTable)["focus"].value_or<std::int64_t>(-1));
        const auto start = (*viewTable)["selectionStart"].value_or<std::int64_t>(0);
        const auto end = (*viewTable)["selectionEnd"].value_or<std::int64_t>(0);
        const session::TrackId track =
            trackFromWire((*viewTable)["selectionTrack"].value_or<std::int64_t>(-1));
        result.view.selection = session::Selection::normalised(
            start, end, project.track(track) != nullptr ? project.track(track)->length() : 0,
            track);
        result.view.selection.channel = int(std::clamp<std::int64_t>((*viewTable)["selectionChannel"].value_or<std::int64_t>(-1), -1, 7));
    }

    if (project.track(result.view.focus) == nullptr) {
        result.view.focus = project.tracks().empty() ? session::kInvalidTrack
                                                     : project.tracks().front().id;
    }

    // --- the Spectral page's state (A22) --------------------------------------------
    if (!spectralFromToml(session["spectral"].as_table(), result.view)) {
        result.error = "invalid harmonic pitch contour";
        return result;
    }
    if (const toml::table* sound = session["sound"].as_table()) {
        for (const auto& [key, node] : *sound) {
            const auto* entry = node.as_table();
            std::int64_t wire = -1;
            const auto text = key.str();
            if (std::from_chars(text.data(), text.data() + text.size(), wire).ec != std::errc{}) continue;
            const auto track = trackFromWire(wire);
            if (!entry || project.track(track) == nullptr) continue;
            for (const auto& [name, value] : *entry)
                if (const auto number = value.value<double>(); number && std::isfinite(*number))
                    result.view.sound[track][std::string(name.str())] = *number;
        }
    }

    if (const auto error = session::validateClipEnvelopes(project); !error.empty()) { result.error = error; return result; }
    result.project = std::move(project);
    result.ok = true;
    return result;
}

bool hasRecovery(const std::string& path)
{
    std::error_code ec;
    const fs::path where = layerDir(fs::path(path), Layer::Recovery);
    return fs::is_regular_file(where / kPendingSave, ec)
        || fs::is_regular_file(where / "session.toml", ec);
}

std::string discardRecovery(const std::string& path)
{
    std::error_code ec;
    const fs::path where = layerDir(fs::path(path), Layer::Recovery);
    if (!fs::exists(where, ec))
        return {};
    const storage::BundleLock lock(fs::path(path), true);
    if (!lock.error().empty()) return lock.error();
    fs::remove_all(where, ec);
    if (ec)
        return "could not remove " + where.string() + ": " + ec.message();
    return storage::syncPath(fs::path(path), true);
}

} // namespace omawave::io
