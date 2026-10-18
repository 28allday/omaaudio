#pragma once

#include "clap/catalogue.h"
#include "vst3/catalogue.h"
#include "host/instance.h"

#include <cstdint>
#include <string>
#include <vector>

struct LilvWorldImpl;
typedef struct LilvWorldImpl LilvWorld;
struct LilvPluginImpl;
typedef struct LilvPluginImpl LilvPlugin;

namespace omawave::lv2 {

// What a plugin can be an insert on, and one catalogue entry: the neutral host
// types (host/instance.h), shared with CLAP since W09.
//
// For LV2, two facts about real plugins decide the layout, and neither is
// guessable from the port count alone. Both were measured against the 360
// plugins installed on the target box:
//
//   * An optional audio port is usually a CHANNEL, not a sidechain. Calf's Compressor
//     is `in_l` required and `in_r` marked lv2:connectionOptional, both in the same
//     port group, designated left and right -- that is a stereo plugin that will also
//     accept mono input. Counting only required ports calls it "1 in, 2 out" and
//     refuses almost all of Calf.
//   * A sidechain is identified by its GROUP, not by the port. LSP's sc_compressor_mono
//     puts its `sc` port in a group carrying pg:sideChainOf. Counting it as a channel
//     would make that mono plugin look stereo and feed a track's right channel into its
//     sidechain, which is silently, plausibly wrong -- the worst kind.
//
// Sidechain ports are counted separately and connected by Chain to external-key
// buffers; the remaining audio ports give a range: at least the required ones, at
// most all of them.
using Layout = host::Layout;
using PluginInfo = host::PluginInfo;
using Format = host::Format;

// The LV2 world and the catalogue derived from it.
//
// Measured on the target box with calf, x42 and lsp-plugins installed: loading the
// world is 10 ms and walking every port of all 360 plugins is 1060 ms. The catalogue
// is the expensive half, which is why this runs on a worker thread once and is then
// kept for the life of the process.
class World {
public:
    World();
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    // Loads every bundle and builds the catalogue. Slow; worker thread only.
    void scan();

    // A bundle every later scan() loads after the LV2 search path: the
    // application's own effects, installed beside it rather than on the user's
    // path. Process-wide; set once at startup, before any scan. Missing
    // directories are skipped.
    static void addBundle(const std::string& directory);
    // Explicit staged VST3 discovery on a private, already scanned worker-owned
    // world. Cancel/limit/options failure preserves the previous catalogue.
    // The staged application worker calls this before publishing its world.
    [[nodiscard]] bool scanVst3(const vst3::CatalogueOptions&, std::vector<std::string>& errors,
                                const std::atomic<bool>* cancel = nullptr);

    [[nodiscard]] bool scanned() const noexcept { return scanned_; }

    // Sorted by name, case-insensitively, because that is the order the browser wants
    // and sorting 360 strings once is cheaper than sorting them on every keystroke.
    [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept
    {
        return plugins_;
    }

    [[nodiscard]] const PluginInfo* find(const std::string& uri) const noexcept;

    // For the phases that instantiate. Null until scan() has run.
    [[nodiscard]] LilvWorld* raw() const noexcept { return world_; }
    [[nodiscard]] const LilvPlugin* lilvPlugin(const std::string& uri) const noexcept;

    // The CLAP plugins the same scan found; their entries are in plugins() too.
    [[nodiscard]] const clap::Catalogue& clap() const noexcept { return clap_; }

private:
    LilvWorld* world_ = nullptr;
    clap::Catalogue clap_;
    bool scanned_ = false;
    std::vector<PluginInfo> plugins_;
};

} // namespace omawave::lv2
