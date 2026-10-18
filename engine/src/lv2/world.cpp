#include "lv2/world.h"

#include "lv2/features.h"

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <cctype>

namespace omawave::lv2 {
namespace {

// The port classes and properties every plugin is measured against. Created once per
// scan rather than per plugin: with 360 plugins and up to a thousand ports each, the
// difference is a second either way.
struct Vocabulary {
    explicit Vocabulary(LilvWorld* world)
        : world(world),
          audioPort(lilv_new_uri(world, LV2_CORE__AudioPort)),
          controlPort(lilv_new_uri(world, LV2_CORE__ControlPort)),
          inputPort(lilv_new_uri(world, LV2_CORE__InputPort)),
          atomPort(lilv_new_uri(world, "http://lv2plug.in/ns/ext/atom#AtomPort")),
          optional(lilv_new_uri(world, LV2_CORE__connectionOptional)),
          portGroup(lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#group")),
          sideChainOf(
              lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#sideChainOf")),
          stateInterface(lilv_new_uri(world, LV2_STATE__interface))
    {
    }

    ~Vocabulary()
    {
        lilv_node_free(audioPort);
        lilv_node_free(controlPort);
        lilv_node_free(inputPort);
        lilv_node_free(atomPort);
        lilv_node_free(optional);
        lilv_node_free(portGroup);
        lilv_node_free(sideChainOf);
        lilv_node_free(stateInterface);
    }

    // A sidechain is a property of the port's GROUP, not of the port. Asking the port
    // for pg:sideChainOf finds nothing on any plugin installed here, which is exactly
    // how a sidechain gets mistaken for a second channel.
    [[nodiscard]] bool isSidechain(const LilvPlugin* plugin, const LilvPort* port) const
    {
        LilvNodes* groups = lilv_port_get_value(plugin, port, portGroup);
        bool sidechain = false;
        LILV_FOREACH (nodes, i, groups) {
            LilvNodes* of = lilv_world_find_nodes(world, lilv_nodes_get(groups, i),
                                                  sideChainOf, nullptr);
            sidechain = sidechain || lilv_nodes_size(of) > 0;
            lilv_nodes_free(of);
        }
        lilv_nodes_free(groups);
        return sidechain;
    }

    Vocabulary(const Vocabulary&) = delete;
    Vocabulary& operator=(const Vocabulary&) = delete;
    Vocabulary(Vocabulary&&) = delete;
    Vocabulary& operator=(Vocabulary&&) = delete;

    LilvWorld* world;
    LilvNode* audioPort;
    LilvNode* controlPort;
    LilvNode* inputPort;
    LilvNode* atomPort;
    LilvNode* optional;
    LilvNode* portGroup;
    LilvNode* sideChainOf;
    LilvNode* stateInterface;
};

std::string nodeString(const LilvNode* node)
{
    if (node == nullptr)
        return {};
    const char* str = lilv_node_as_string(node);
    return str != nullptr ? std::string(str) : std::string{};
}

// Takes ownership: lilv hands back a node the caller must free.
std::string takeNodeString(LilvNode* node)
{
    std::string out = nodeString(node);
    lilv_node_free(node);
    return out;
}

// A port count said the way the browser will show it. Ranges, because a port marked
// optional makes the count a range rather than a number.
std::string range(std::uint32_t required, std::uint32_t total)
{
    if (required == total)
        return std::to_string(total);
    return std::to_string(required) + "-" + std::to_string(total);
}

std::string layoutDescription(const PluginInfo& info)
{
    if (info.totalIn == 0 && info.totalOut == 0)
        return "no audio ports";
    return range(info.requiredIn, info.totalIn) + " in, "
         + range(info.requiredOut, info.totalOut) + " out";
}

void classify(PluginInfo& info)
{
    const bool mono = info.fits(1);
    const bool stereo = info.fits(2);

    if (mono && stereo)
        info.layout = Layout::MonoOrStereo;
    else if (mono)
        info.layout = Layout::Mono;
    else if (stereo)
        info.layout = Layout::Stereo;
    else
        info.layout = Layout::Unsupported;

    // A missing feature is the more useful thing to say when both are true, so it is
    // not overwritten: "needs a feature Omawave does not provide" tells you the host is
    // the limit, where a port count reads as the plugin being the wrong shape.
    if (info.layout == Layout::Unsupported && info.unusableReason.empty())
        info.unusableReason = "not an insert: " + layoutDescription(info);

    info.usable = info.unusableReason.empty();
}

PluginInfo describe(const LilvPlugin* plugin, const Vocabulary& vocab)
{
    PluginInfo info;
    info.uri = nodeString(lilv_plugin_get_uri(plugin));
    info.name = takeNodeString(lilv_plugin_get_name(plugin));
    info.author = takeNodeString(lilv_plugin_get_author_name(plugin));

    if (const LilvPluginClass* cls = lilv_plugin_get_class(plugin); cls != nullptr)
        info.className = nodeString(lilv_plugin_class_get_label(cls));

    const std::uint32_t ports = lilv_plugin_get_num_ports(plugin);
    for (std::uint32_t i = 0; i < ports; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        const bool isInput = lilv_port_is_a(plugin, port, vocab.inputPort);

        if (lilv_port_is_a(plugin, port, vocab.audioPort)) {
            if (vocab.isSidechain(plugin, port)) {
                // Never connected in this phase, and never counted as a channel.
                // Routing one is a feature the session model cannot express yet.
                if (isInput)
                    ++info.sidechainIn;
                continue;
            }
            const bool isOptional = lilv_port_has_property(plugin, port, vocab.optional);
            if (isInput) {
                ++info.totalIn;
                if (!isOptional)
                    ++info.requiredIn;
            } else {
                ++info.totalOut;
                if (!isOptional)
                    ++info.requiredOut;
            }
        } else if (lilv_port_is_a(plugin, port, vocab.controlPort)) {
            if (isInput)
                ++info.controlIn;
        } else if (lilv_port_is_a(plugin, port, vocab.atomPort)) {
            ++info.atomPorts;
        }
    }

    // A required feature we do not provide is the one refusal worth being precise
    // about: it is the difference between "this host cannot run it" and "this plugin
    // is broken", and the browser says which.
    LilvNodes* required = lilv_plugin_get_required_features(plugin);
    LILV_FOREACH (nodes, i, required) {
        const std::string uri = nodeString(lilv_nodes_get(required, i));
        if (uri == LV2_WORKER__schedule)
            info.needsWorker = true;
        if (!isProvidedFeature(uri) && info.unusableReason.empty())
            info.unusableReason = "needs a feature Omawave does not provide: " + uri;
    }
    lilv_nodes_free(required);

    info.hasStateInterface =
        lilv_plugin_has_extension_data(plugin, vocab.stateInterface);

    classify(info);
    return info;
}

bool byNameFold(const PluginInfo& a, const PluginInfo& b)
{
    const auto fold = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    };
    const auto less = [&fold](const std::string& x, const std::string& y) {
        return std::lexicographical_compare(
            x.begin(), x.end(), y.begin(), y.end(),
            [&fold](char l, char r) { return fold(static_cast<unsigned char>(l))
                                           < fold(static_cast<unsigned char>(r)); });
    };
    if (less(a.name, b.name))
        return true;
    if (less(b.name, a.name))
        return false;
    return a.uri < b.uri;    // two plugins really can share a name
}

} // namespace

World::World() : world_(lilv_world_new()) {}

World::~World()
{
    if (world_ != nullptr)
        lilv_world_free(world_);
}

namespace {
std::mutex& bundleMutex() { static std::mutex m; return m; }
std::vector<std::string>& extraBundles() { static std::vector<std::string> b; return b; }
}

void World::addBundle(const std::string& directory)
{
    const std::lock_guard lock(bundleMutex());
    auto& bundles = extraBundles();
    if (std::find(bundles.begin(), bundles.end(), directory) == bundles.end()) bundles.push_back(directory);
}

void World::scan()
{
    if (world_ == nullptr || scanned_)
        return;

    lilv_world_load_all(world_);
    std::vector<std::string> bundles;
    { const std::lock_guard lock(bundleMutex()); bundles = extraBundles(); }
    for (std::string directory : bundles) {
        if (directory.empty() || !std::filesystem::is_directory(directory)) continue;
        if (directory.back() != '/') directory += '/';
        if (LilvNode* uri = lilv_new_file_uri(world_, nullptr, directory.c_str())) {
            lilv_world_load_bundle(world_, uri);
            lilv_node_free(uri);
        }
    }

    const Vocabulary vocab(world_);
    const LilvPlugins* found = lilv_world_get_all_plugins(world_);

    plugins_.clear();
    plugins_.reserve(lilv_plugins_size(found));
    LILV_FOREACH (plugins, i, found)
        plugins_.push_back(describe(lilv_plugins_get(found, i), vocab));

    // CLAP plugins join the same list under their own "clap:" identity.
    clap_.scan();
    for (const PluginInfo& p : clap_.plugins()) plugins_.push_back(p);

    std::sort(plugins_.begin(), plugins_.end(), byNameFold);
    scanned_ = true;
}

bool World::scanVst3(const vst3::CatalogueOptions& options, std::vector<std::string>& errors,
                     const std::atomic<bool>* cancel)
{
    errors.clear();
    if (!scanned_) { errors.emplace_back("Scan the base catalogue before VST3 discovery"); return false; }
    auto result = vst3::scanCatalogue(options, cancel);
    errors = std::move(result.errors);
    if (!result.ok) return false;
    std::vector<PluginInfo> combined;
    combined.reserve(plugins_.size() + result.plugins.size());
    for (const auto& plugin : plugins_) if (plugin.format != Format::Vst3) combined.push_back(plugin);
    for (auto& plugin : result.plugins) combined.push_back(std::move(plugin));
    std::sort(combined.begin(), combined.end(), byNameFold);
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    plugins_ = std::move(combined);
    return true;
}

const PluginInfo* World::find(const std::string& uri) const noexcept
{
    for (const PluginInfo& info : plugins_) {
        if (info.uri == uri)
            return &info;
    }
    return nullptr;
}

const LilvPlugin* World::lilvPlugin(const std::string& uri) const noexcept
{
    if (world_ == nullptr || !scanned_)
        return nullptr;

    LilvNode* node = lilv_new_uri(world_, uri.c_str());
    if (node == nullptr)
        return nullptr;
    const LilvPlugin* plugin =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), node);
    lilv_node_free(node);
    return plugin;
}

} // namespace omawave::lv2
