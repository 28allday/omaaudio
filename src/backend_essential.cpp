#include "backend.h"
#include "session/elastic.h"
#include "session/routing.h"
#include "pluginmetadata.h"

#include <algorithm>
#include <cmath>

namespace {
enum class Unit { Plain, Decibels, Percent };
struct Field {
    const char *symbol, *label, *unit;
    Unit conversion = Unit::Plain;
    int decimals = 2;
    const char *group = "";
    bool choice = false;
};
struct FixedControl {
    const char *symbol;
    float value;
    bool choice = false;
    bool toggle = false;
};
struct Processor {
    const char *id, *title, *uri, *pluginName;
    std::vector<Field> fields;
    QVariantMap gentle, firm;
    const char *gentleName = "Gentle", *firmName = "Firm";
    const char *description = "";
    const char *enableSymbol = "bypass";
    float enabledValue = 0;
    bool sidechain = false;
    std::vector<FixedControl> setup{};
};
const Processor *processor(const QString &id)
{
    static const std::vector<Processor> processors{
        {"dynamics", "Dynamics", "http://calf.sourceforge.net/plugins/Compressor", "Calf Compressor",
            {{"threshold", "Threshold", "dB", Unit::Decibels}, {"ratio", "Ratio", ":1"},
             {"attack", "Attack", "ms"}, {"release", "Release", "ms"},
             {"makeup", "Makeup", "dB", Unit::Decibels}, {"mix", "Mix", "%", Unit::Percent}},
            {{"threshold", -18.0}, {"ratio", 2.0}, {"attack", 15.0}, {"release", 150.0}, {"makeup", 0.0}, {"mix", 100.0}},
            {{"threshold", -24.0}, {"ratio", 4.0}, {"attack", 8.0}, {"release", 120.0}, {"makeup", 0.0}, {"mix", 100.0}}},
        {"gate", "Noise Gate", "http://calf.sourceforge.net/plugins/Gate", "Calf Gate",
            {{"threshold", "Threshold", "dB", Unit::Decibels},
             {"range", "Closed gain", "dB", Unit::Decibels}, {"ratio", "Ratio", ":1"},
             {"attack", "Attack", "ms"}, {"release", "Release", "ms"},
             {"makeup", "Makeup", "dB", Unit::Decibels}},
            {{"threshold", -40.0}, {"range", -18.0}, {"ratio", 2.0}, {"attack", 5.0}, {"release", 250.0}, {"makeup", 0.0}},
            {{"threshold", -35.0}, {"range", -48.0}, {"ratio", 10.0}, {"attack", 2.0}, {"release", 150.0}, {"makeup", 0.0}},
            "Gentle", "Firm", "Turns down quiet sound between phrases. It can also affect quiet speech and the ends of words; it cannot remove noise under speech. Closed gain sets how far it can turn the sound down. Compare with Bypass."},
        {"deesser", "De-Esser", "http://calf.sourceforge.net/plugins/Deesser", "Calf Deesser",
            {{"threshold", "Threshold", "dB", Unit::Decibels}, {"ratio", "Ratio", ":1"},
             {"laxity", "Laxity", ""}, {"f1_freq", "Split frequency", "Hz"},
             {"f2_freq", "Detector peak", "Hz"}, {"makeup", "Makeup", "dB", Unit::Decibels}},
            {{"threshold", -18.0}, {"ratio", 2.0}, {"laxity", 15.0}, {"f1_freq", 6000.0}, {"f2_freq", 4500.0}, {"makeup", 0.0}},
            {{"threshold", -24.0}, {"ratio", 4.0}, {"laxity", 10.0}, {"f1_freq", 5500.0}, {"f2_freq", 5000.0}, {"makeup", 0.0}}},
        {"eq", "EQ", "http://calf.sourceforge.net/plugins/Equalizer5Band", "Calf Equalizer 5 Band",
            {
             {"ls_active", "Channels", "", Unit::Plain, 0, "Low shelf", true},
             {"ls_freq", "Frequency", "Hz", Unit::Plain, 1, "Low shelf"},
             {"ls_level", "Gain", "dB", Unit::Decibels, 2, "Low shelf"},
             {"ls_q", "Q", "", Unit::Plain, 2, "Low shelf"},
             {"p1_active", "Channels", "", Unit::Plain, 0, "Bell 1", true},
             {"p1_freq", "Frequency", "Hz", Unit::Plain, 1, "Bell 1"},
             {"p1_level", "Gain", "dB", Unit::Decibels, 2, "Bell 1"},
             {"p1_q", "Q", "", Unit::Plain, 2, "Bell 1"},
             {"p2_active", "Channels", "", Unit::Plain, 0, "Bell 2", true},
             {"p2_freq", "Frequency", "Hz", Unit::Plain, 1, "Bell 2"},
             {"p2_level", "Gain", "dB", Unit::Decibels, 2, "Bell 2"},
             {"p2_q", "Q", "", Unit::Plain, 2, "Bell 2"},
             {"p3_active", "Channels", "", Unit::Plain, 0, "Bell 3", true},
             {"p3_freq", "Frequency", "Hz", Unit::Plain, 1, "Bell 3"},
             {"p3_level", "Gain", "dB", Unit::Decibels, 2, "Bell 3"},
             {"p3_q", "Q", "", Unit::Plain, 2, "Bell 3"},
             {"hs_active", "Channels", "", Unit::Plain, 0, "High shelf", true},
             {"hs_freq", "Frequency", "Hz", Unit::Plain, 1, "High shelf"},
             {"hs_level", "Gain", "dB", Unit::Decibels, 2, "High shelf"},
             {"hs_q", "Q", "", Unit::Plain, 2, "High shelf"},
             {"level_out", "Output gain", "dB", Unit::Decibels, 2, "Output"}},
            {{"ls_active", 1.0}, {"ls_freq", 120.0}, {"ls_level", 2.5}, {"hs_active", 1.0}, {"hs_freq", 6000.0}, {"hs_level", -1.0}},
            {{"ls_active", 1.0}, {"ls_level", -1.0}, {"p2_active", 1.0}, {"p2_freq", 2000.0}, {"p2_level", 3.0}, {"p2_q", 0.8}},
            "Warmth", "Presence"},
        {"clarity", "Brightness", "http://calf.sourceforge.net/plugins/Exciter", "Calf Exciter",
            {{"freq", "Scope frequency", "Hz", Unit::Plain, 1},
             {"drive", "Harmonics", ""}, {"blend", "Harmonic blend", ""},
             {"amount", "Effect amount", "%", Unit::Percent},
             {"level_in", "Input gain", "dB", Unit::Decibels},
             {"level_out", "Output gain", "dB", Unit::Decibels}},
            {{"freq", 3500.0}, {"drive", 2.0}, {"blend", 0.0}, {"amount", 10.0}},
            {{"freq", 2500.0}, {"drive", 4.0}, {"blend", 0.0}, {"amount", 25.0}},
            "Gentle", "Firm", "Adds brightness by creating upper harmonics: higher tones related to the sound. Use a small amount and compare with Bypass. This effect does not remove background noise."},
        {"ducking", "Ducking", "http://lsp-plug.in/plugins/lv2/sc_compressor_stereo", "LSP Sidechain Compressor Stereo",
            {{"al", "Threshold", "dB", Unit::Decibels}, {"cr", "Ratio", ":1"},
             {"at", "Attack", "ms"}, {"rt", "Release", "ms"}, {"hold", "Hold", "ms"},
             {"mk", "Makeup", "dB", Unit::Decibels}},
            {{"al", -18.0}, {"cr", 3.0}, {"at", 15.0}, {"rt", 400.0}, {"hold", 100.0}, {"mk", 0.0}},
            {{"al", -24.0}, {"cr", 8.0}, {"at", 8.0}, {"rt", 600.0}, {"hold", 150.0}, {"mk", 0.0}},
            "Gentle", "Firm", "Turns this track down while the trigger track plays, for example music under speech. The trigger track's volume, effects and Mute affect the result. Apply sets up the compressor to use that trigger on both stereo channels.",
            "enabled", 1, true,
            {{"sct", 2, true}, {"cm", 0, true}, {"scm", 1, true}, {"scs", 5, true},
             {"ssplit", 0, false, true}, {"scl", 0, false, true}, {"scp", 1},
             {"cdr", 0}, {"cwt", 1}, {"cdw", 100},
             {"in2sc", 0}, {"lk2sc", 0}, {"sc2in", 0}}},
        // No internal enable: the effect's own bypass in the rack turns it off.
        {"pitch", "Pitch Correction", "http://gareus.org/oss/lv2/fat1#scales", "x42-Autotune",
            {{"scale", "Scale", "", Unit::Plain, 0, "", true},
             {"corr", "Correction", "%", Unit::Percent, 0},
             {"filter", "Response", "s", Unit::Plain, 2},
             {"bias", "Note hold", "%", Unit::Percent, 0},
             {"offset", "Pitch offset", "st", Unit::Plain, 2},
             {"tuning", "Reference A", "Hz", Unit::Plain, 1}},
            {{"corr", 50.0}, {"filter", 0.2}, {"bias", 50.0}},
            {{"corr", 100.0}, {"filter", 0.02}, {"bias", 80.0}},
            "Natural", "Strong",
            "Moves a sung note towards the nearest note of the chosen scale. Natural keeps slides and vibrato; Strong snaps quickly for an obvious effect. Works on one voice at a time; compare with Bypass.",
            ""}
    };
    for (const auto &p : processors) if (id == QLatin1String(p.id)) return &p;
    return nullptr;
}
double displayValue(double raw, Unit unit)
{
    return unit == Unit::Decibels ? 20 * std::log10(raw) : unit == Unit::Percent ? raw * 100 : raw;
}
double portValue(double shown, Unit unit)
{
    return unit == Unit::Decibels ? std::pow(10, shown / 20) : unit == Unit::Percent ? shown / 100 : shown;
}
}

QVariantMap Backend::essentialProcessor(const QString &module, int track, int slot)
{
    QVariantMap out{{"available", false}, {"module", module}, {"stamp", automationStamp()}};
    const auto reject = [&](const QString &reason) { out["reason"] = reason; return out; };
    const auto *spec = processor(module);
    if (!spec) return reject(tr("Choose a supported Quick Fixes processor."));
    out["title"] = tr(spec->title); out["uri"] = QString::fromLatin1(spec->uri);
    out["description"] = *spec->description ? tr(spec->description) : QString{};
    out["requiresSidechain"] = spec->sidechain;
    const auto *t = m_project.track(trackArg(track));
    if (!t) return reject(tr("Click a track before opening this processor."));
    out["track"] = int(t->id); out["trackName"] = QString::fromStdString(t->name);
    if (m_scanning || !m_lv2->scanned()) return reject(tr("Read the installed plugin catalogue to check availability."));
    const auto *info = m_lv2->find(spec->uri);
    // Mono-only processors run once per channel (dual mono), as in the rack.
    if (!info || !info->usable || !(info->fits(kOutputChannels) || (!spec->sidechain && info->fits(1)))) return reject(info ? (info->usable ? tr("This plugin does not support the track’s channel layout.") : QString::fromStdString(info->unusableReason))
        : tr("This module needs the optional %1 LV2 plugin. Install it to use these controls.").arg(QString::fromLatin1(spec->pluginName)));
    out["processor"] = QString::fromStdString(info->name);
    if (spec->sidechain && info->sidechainIn != kOutputChannels)
        return reject(tr("The installed processor does not expose compatible external sidechain inputs."));
    QVariantList choices;
    for (std::size_t i = 0; i < t->inserts.size(); ++i) if (t->inserts[i].uri == spec->uri)
        choices.append(QVariantMap{{"slot", int(i)}, {"label", tr("Effect %1 · %2").arg(i + 1).arg(QString::fromStdString(info->name))}});
    out["slots"] = choices;
    if (slot == -2) slot = choices.empty() ? -1 : choices.first().toMap().value("slot").toInt();
    if (slot < -1 || (slot >= 0 && (std::size_t(slot) >= t->inserts.size() || t->inserts[std::size_t(slot)].uri != spec->uri)))
        return reject(tr("The chosen effect changed. Reload this processor."));
    out["slot"] = slot;
    if (spec->sidechain) {
        QVariantList sources;
        for (const auto &source : m_project.tracks()) if (source.id != t->id)
            sources.append(QVariantMap{{"track", int(source.id)}, {"name", QString::fromStdString(source.name)}});
        out["sidechainChoices"] = sources;
        out["sidechain"] = t->routing.sidechain == session::kInvalidTrack ? -1 : int(t->routing.sidechain);
        int otherKeys = 0;
        for (std::size_t i = 0; i < t->inserts.size(); ++i) if (int(i) != slot) {
            const auto *effect = m_lv2->find(t->inserts[i].uri);
            if (effect && effect->sidechainIn > 0) ++otherKeys;
        }
        out["otherSidechainInserts"] = otherKeys;
    }
    if (slot == -1 && m_pluginLibrary.disabled().contains(QString::fromLatin1(spec->uri)))
        return reject(tr("This plugin is disabled for new effects. Enable it in the Plug-in Manager first."));
    std::string error;
    auto probe = host::createInstance(*m_lv2, spec->uri, hostFeatures(), error);
    if (!probe) return reject(QString::fromStdString(error));
    const session::Insert *insert = slot >= 0 ? &t->inserts[std::size_t(slot)] : nullptr;
    out["enabled"] = !insert || !insert->bypass;
    QVariantList fields;
    QVariantMap defaults;
    for (const auto &field : spec->fields) {
        const auto found = std::find_if(probe->controls().begin(), probe->controls().end(),
            [&](const auto &p) { return !p.output && p.symbol == field.symbol; });
        if (found == probe->controls().end() || !std::isfinite(found->minimum) || !std::isfinite(found->maximum)
            || !std::isfinite(found->defaultValue) || found->maximum <= found->minimum
            || (field.conversion == Unit::Decibels && found->minimum <= 0))
            return reject(tr("The installed plugin has incompatible controls. Use its controls in the Track Effects panel."));
        QVariantList controlChoices;
        if (field.choice) {
            const auto *plugin = m_lv2->lilvPlugin(spec->uri);
            controlChoices = pluginControlPresentation(m_lv2->raw(), plugin,
                (plugin ? lilv_plugin_get_port_by_index(plugin, found->index) : nullptr), found->minimum, found->maximum).value("choices").toList();
            if (controlChoices.isEmpty()) return reject(tr("The installed plugin has incompatible channel-mode choices. Use its controls in the Track Effects panel."));
            for (auto &entry : controlChoices) {
                auto value = entry.toMap();
                if (value.value("value").toDouble() == 0 && value.value("label").toString() == "0") value["label"] = tr("Off");
                entry = value;
            }
        }
        const auto *stored = insert ? insert->control(found->index) : nullptr;
        const double current = stored ? stored->value : found->defaultValue;
        if (!std::isfinite(current) || current < found->minimum || current > found->maximum
            || (found->integer && current != std::round(current)))
            return reject(tr("An existing control is outside this plugin’s range. Correct it in the Track Effects panel first."));
        if (!controlChoices.isEmpty() && std::none_of(controlChoices.cbegin(), controlChoices.cend(),
            [&](const auto &entry) { return entry.toMap().value("value").toDouble() == current; }))
            return reject(tr("An existing channel mode is not supported by this plugin. Correct it in the Track Effects panel first."));
        defaults[QString::fromLatin1(field.symbol)] = displayValue(found->defaultValue, field.conversion);
        const int decimals = found->integer ? 0 : field.decimals;
        const double precision = std::pow(10, decimals);
        fields.append(QVariantMap{{"symbol", QString::fromLatin1(field.symbol)}, {"label", tr(field.label)},
            {"group", *field.group ? tr(field.group) : QString{}}, {"choices", controlChoices},
            {"unit", QString::fromLatin1(field.unit)}, {"decimals", decimals}, {"integer", found->integer}, {"port", int(found->index)},
            {"value", displayValue(current, field.conversion)}, {"defaultValue", displayValue(found->defaultValue, field.conversion)},
            {"minimum", std::round(displayValue(found->minimum, field.conversion) * precision) / precision},
            {"maximum", std::round(displayValue(found->maximum, field.conversion) * precision) / precision},
            {"rawMin", double(found->minimum)}, {"rawMax", double(found->maximum)}, {"rawDefault", double(found->defaultValue)},
            {"automated", stored && stored->automationActive()}});
    }
    QVariantList configuration;
    bool configurationAutomated = false;
    for (const auto &setting : spec->setup) {
        const auto found = std::find_if(probe->controls().begin(), probe->controls().end(),
            [&](const auto &p) { return !p.output && p.symbol == setting.symbol; });
        if (found == probe->controls().end() || !std::isfinite(found->minimum) || !std::isfinite(found->maximum)
            || !std::isfinite(found->defaultValue) || setting.value < found->minimum || setting.value > found->maximum
            || (setting.toggle && (!found->toggled || found->minimum != 0 || found->maximum != 1)))
            return reject(tr("The installed processor has incompatible ducking controls. Use its controls in the Track Effects panel."));
        if (setting.choice) {
            const auto *plugin = m_lv2->lilvPlugin(spec->uri);
            const auto options = pluginControlPresentation(m_lv2->raw(), plugin,
                (plugin ? lilv_plugin_get_port_by_index(plugin, found->index) : nullptr), found->minimum, found->maximum).value("choices").toList();
            if (std::none_of(options.cbegin(), options.cend(), [&](const auto &entry) {
                    return entry.toMap().value("value").toDouble() == setting.value;
                })) return reject(tr("The installed processor has incompatible ducking choices. Use its controls in the Track Effects panel."));
        }
        const auto *stored = insert ? insert->control(found->index) : nullptr;
        const auto current = stored ? stored->value : found->defaultValue;
        if (!std::isfinite(current) || current < found->minimum || current > found->maximum)
            return reject(tr("A ducking setup control is outside this plugin’s range. Correct it in the Track Effects panel first."));
        const bool automated = stored && stored->automationActive();
        configurationAutomated = configurationAutomated || automated;
        configuration.append(QVariantMap{{"port", int(found->index)}, {"symbol", QString::fromLatin1(setting.symbol)},
            {"value", current}, {"requiredValue", setting.value}, {"automated", automated}});
    }
    out["configuration"] = configuration;
    out["configurationAutomated"] = configurationAutomated;
    if (*spec->enableSymbol) {
        const auto bypass = std::find_if(probe->controls().begin(), probe->controls().end(),
            [&](const auto &p) { return !p.output && p.symbol == spec->enableSymbol && p.minimum == 0 && p.maximum == 1 && p.toggled; });
        if (bypass == probe->controls().end()) return reject(tr("The installed plugin has an incompatible bypass control."));
        const auto *storedBypass = insert ? insert->control(bypass->index) : nullptr;
        const auto currentEnable = storedBypass ? storedBypass->value : bypass->defaultValue;
        if (!std::isfinite(currentEnable)) return reject(tr("Correct the processor’s internal enable control in the Track Effects panel first."));
        if (std::abs(currentEnable - spec->enabledValue) >= 0.5f) out["enabled"] = false;
        out["bypassPort"] = int(bypass->index);
        out["enableValue"] = spec->enabledValue;
        out["enableCurrent"] = currentEnable;
        out["bypassAutomated"] = storedBypass && storedBypass->automationActive();
    } else {
        out["bypassPort"] = -1;   // only the rack's bypass applies
        out["bypassAutomated"] = false;
    }
    out["fields"] = fields;
    const auto startingValues = [&](const QVariantMap &overrides) {
        auto values = defaults;
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) values[it.key()] = it.value();
        return values;
    };
    out["presets"] = QVariantList{QVariantMap{{"name", tr(spec->gentleName)}, {"values", startingValues(spec->gentle)}},
                                  QVariantMap{{"name", tr(spec->firmName)}, {"values", startingValues(spec->firm)}}};
    out["available"] = true;
    return out;
}

bool Backend::applyEssentialProcessor(const QString &module, int track, int slot,
    const QString &stamp, bool enabled, const QVariantMap &values, int sidechain)
{
    if (const auto deferred = deferAutomationEdit([this, module, track, slot, stamp, enabled, values, sidechain] { return applyEssentialProcessor(module, track, slot, stamp, enabled, values, sidechain); })) return *deferred;
    if (m_shuttingDown || m_recording || automationWriting() || m_loading || m_scanning
        || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current edit before applying this processor.")); return false;
    }
    if (track < 0 || slot < -1 || !automationStampCurrent(stamp)) {
        setError(tr("The track or effect changed. Reload this processor before applying.")); return false;
    }
    const auto descriptor = essentialProcessor(module, track, slot);
    if (!descriptor.value("available").toBool()) { setError(descriptor.value("reason").toString()); return false; }
    const auto fields = descriptor.value("fields").toList();
    if (values.size() != fields.size()) { setError(tr("Supply every displayed processor control.")); return false; }
    const auto *spec = processor(module);
    auto *t = m_project.track(static_cast<session::TrackId>(track));
    auto routing = t->routing;
    if (spec->sidechain) {
        if (sidechain < 0 || !m_project.track(static_cast<session::TrackId>(sidechain)) || sidechain == track) {
            setError(tr("Choose another track as the ducking trigger.")); return false;
        }
        routing.sidechain = static_cast<session::TrackId>(sidechain);
        auto snapshot = m_project; snapshot.track(t->id)->routing = routing;
        const auto graph = session::compileRouting(snapshot);
        if (!graph.ok) { setError(QString::fromStdString(graph.error)); return false; }
    } else if (sidechain != -1) {
        setError(tr("Only Ducking accepts a trigger track.")); return false;
    }
    auto proposed = t->inserts;
    if (slot == -1) { session::Insert insert; insert.uri = spec->uri; proposed.push_back(std::move(insert)); }
    auto &insert = proposed[slot < 0 ? proposed.size() - 1 : std::size_t(slot)];
    for (qsizetype i = 0; i < fields.size(); ++i) {
        const auto field = fields[i].toMap(); const auto symbol = field.value("symbol").toString();
        bool valid = false; const double value = values.value(symbol).toDouble(&valid);
        if (!values.contains(symbol) || !valid || !std::isfinite(value) || values.value(symbol).metaType().id() == QMetaType::Bool
            || value < field.value("minimum").toDouble() || value > field.value("maximum").toDouble()
            || (field.value("integer").toBool() && value != std::round(value))) {
            setError(tr("Enter a valid value for %1.").arg(field.value("label").toString())); return false;
        }
        const auto controlChoices = field.value("choices").toList();
        if (!controlChoices.isEmpty() && std::none_of(controlChoices.cbegin(), controlChoices.cend(),
            [&](const auto &entry) { return entry.toMap().value("value").toDouble() == value; })) {
            setError(tr("Choose a listed value for %1.").arg(field.value("label").toString())); return false;
        }
        const auto port = field.value("port").toUInt();
        const double tolerance = 0.5 / std::pow(10, field.value("decimals").toInt()) + 1e-9;
        if (std::abs(value - field.value("value").toDouble()) <= tolerance) continue;
        if (field.value("automated").toBool()) {
            setError(tr("%1 is automated. Edit or disable its automation first.").arg(field.value("label").toString())); return false;
        }
        const double raw = std::abs(value - field.value("defaultValue").toDouble()) <= tolerance
            ? field.value("rawDefault").toDouble() : portValue(value, spec->fields[std::size_t(i)].conversion);
        insert.setControl(port, static_cast<float>(std::clamp(raw, field.value("rawMin").toDouble(), field.value("rawMax").toDouble())));
    }
    const int bypassPort = descriptor.value("bypassPort").toInt();
    if (descriptor.value("bypassAutomated").toBool() && enabled) {
        setError(tr("The plugin’s internal bypass is automated. Edit or disable that automation first.")); return false;
    }
    if (enabled && descriptor.value("configurationAutomated").toBool()) {
        setError(tr("The ducking setup has automation. Edit or disable it in the Track Effects panel before enabling Ducking.")); return false;
    }
    if (enabled) {
        for (const auto &entry : descriptor.value("configuration").toList()) {
            const auto setting = entry.toMap();
            if (setting.value("value").toFloat() != setting.value("requiredValue").toFloat())
                insert.setControl(setting.value("port").toUInt(), setting.value("requiredValue").toFloat());
        }
        if (bypassPort >= 0 && descriptor.value("enableCurrent").toFloat() != descriptor.value("enableValue").toFloat())
            insert.setControl(std::uint32_t(bypassPort), descriptor.value("enableValue").toFloat());
    }
    insert.bypass = !enabled;
    if (proposed == t->inserts && routing == t->routing) { setError({}); return true; }

    if (const auto error = session::validateElastic(m_project); !error.empty()) {
        setError(QString::fromStdString(error)); return false;
    }

    // Warm a complete replacement before changing the model or publishing a plan.
    // A failed active slot leaves the current chain and Undo history intact.
    auto prepared = std::make_unique<lv2::Chain>(kOutputChannels, lv2::kMaxBlock);
    for (const auto &effect : proposed) {
        std::string error;
        if (!prepared->append(*m_lv2, hostFeatures(), effect, error)) {
            if (!effect.bypass) { setError(QString::fromStdString(error)); return false; }
            prepared->appendMissing(effect.identity);
        }
    }
    if (!prepared->warmUp(nullptr)) { setError(tr("Effect warm-up failed.")); return false; }
    const auto before = captureEditState();
    setError({});
    t->inserts = std::move(proposed);
    t->routing = std::move(routing);
    const auto key = session::EffectOwner::track(t->id);
    if (auto old = m_chains.find(key); old != m_chains.end() && old->second)
        m_retiredChains.push_back(std::move(old->second));
    m_chains[key] = std::move(prepared); m_chainSpecs[key] = t->inserts;
    commit(std::string("apply ") + spec->title, t->id, before);
    if (slot == -1) {
        if (const auto error = m_pluginLibrary.remember(QString::fromLatin1(spec->uri)); !error.isEmpty()) setError(error);
        else emit pluginLibraryChanged();
    }
    if (m_pluginPortsTarget == track && m_pluginPortsSlot >= 0) requestPluginPorts(m_pluginPortsSlot, track);
    return true;
}
