#include "backend.h"
#include "builtinpresets.h"
#include "pluginmetadata.h"
#include "session/elastic.h"
#include "session/effect_racks.h"
#include "io/plugin_state.h"
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {
bool acceptsValue(const QVariantMap &field, double value)
{
    if (!std::isfinite(value) || value < field.value("minimum").toDouble() || value > field.value("maximum").toDouble()
        || (field.value("integer").toBool() && value != std::round(value))) return false;
    const auto choices = field.value("choices").toList();
    return choices.isEmpty() || std::any_of(choices.cbegin(), choices.cend(), [&](const auto &choice) {
        return choice.toMap().value("value").toDouble() == value;
    });
}

// A starting point's value as this control can hold it: inside its range, a
// whole number or a switch where it must be, the nearest of its named choices.
double fitValue(const QVariantMap &field, double value)
{
    const double low = field.value("minimum").toDouble(), high = field.value("maximum").toDouble();
    value = std::clamp(std::isfinite(value) ? value : field.value("default").toDouble(), low, high);
    if (field.value("integer").toBool() || field.value("toggled").toBool()) value = std::clamp(std::round(value), low, high);
    const auto choices = field.value("choices").toList();
    if (!choices.isEmpty()) {
        double best = choices.front().toMap().value("value").toDouble();
        for (const auto &choice : choices) {
            const double v = choice.toMap().value("value").toDouble();
            if (std::abs(v - value) < std::abs(best - value)) best = v;
        }
        value = best;
    }
    return value;
}
}

QVariantList Backend::presetFields(const session::Insert &insert, QString &reason)
{
    const auto reject = [&](const QString &why) { reason = why; return QVariantList{}; };
    if (!m_lv2->scanned()) return reject(tr("Read the installed plugin catalogue first."));
    const auto *info = m_lv2->find(insert.uri);
    if (!info || !info->usable) return reject(tr("This processor is missing or unavailable on this system."));
    std::string error;
    auto probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error, insert.legacyClapControls,
        insert.instanceState.empty() ? nullptr : &insert.instanceState.front());
    if (!probe) return reject(QString::fromStdString(error));
    const auto *plugin = m_lv2->lilvPlugin(insert.uri);
    QVariantList fields;
    QSet<QString> symbols;
    for (const auto &port : probe->controls()) {
        if (port.output) continue;
        const auto legacySymbol = QString::fromStdString(port.symbol);
        const auto symbol = port.parameterId ? QString::number(*port.parameterId) : legacySymbol;
        if (symbol.isEmpty() || symbols.contains(symbol) || fields.size() >= 4096
            || !std::isfinite(port.minimum) || !std::isfinite(port.maximum) || port.minimum > port.maximum)
            return reject(tr("This processor has incompatible parameter metadata."));
        const auto *stored = insert.control(port.index);
        const auto current = stored ? stored->value : probe->parameterValue(port.index);
        QVariantMap field{{"symbol", symbol}, {"port", port.index}, {"name", QString::fromStdString(port.name)},
            {"minimum", port.minimum}, {"maximum", port.maximum}, {"integer", port.integer}, {"toggled", port.toggled},
            {"default", port.clamp(port.defaultValue)}, {"value", current},
            {"legacySymbol", legacySymbol}, {"automated", stored && stored->automationActive()}, {"choices", pluginControlPresentation(m_lv2->raw(), plugin,
                (plugin ? lilv_plugin_get_port_by_index(plugin, port.index) : nullptr), port.minimum, port.maximum).value("choices")}};
        if (!acceptsValue(field, current)) return reject(tr("Correct the out-of-range %1 control in the rack first.").arg(field.value("name").toString()));
        symbols.insert(symbol); fields.append(field);
    }
    if (fields.isEmpty()) return reject(tr("This processor has no input control parameters to save."));
    reason.clear();
    return fields;
}

void Backend::listPresets(const QString &uri, const QVariantList &fields, QVariantMap &out)
{
    QSet<QString> symbols;
    for (const auto &entry : fields) symbols.insert(entry.toMap().value("legacySymbol").toString());
    // OmaAudio's starting points for this effect, each with the controls it names
    // that this version of the effect does not have.
    QVariantList builtIn;
    for (const auto *preset : builtInPresetsFor(uri)) {
        QStringList skipped;
        int matched = 0;
        for (auto it = preset->controls.cbegin(); it != preset->controls.cend(); ++it)
            symbols.contains(it.key()) ? ++matched : (skipped << it.key(), 0);
        if (matched == 0) continue;
        builtIn.append(QVariantMap{{"id", preset->id}, {"name", preset->name}, {"group", preset->group},
                                   {"note", preset->note}, {"skipped", skipped}});
    }
    // The effect's own presets, read once per effect.
    auto own = m_ownPluginPresets.constFind(uri);
    if (own == m_ownPluginPresets.cend())
        own = m_ownPluginPresets.insert(uri, pluginOwnPresets(m_lv2->raw(), m_lv2->lilvPlugin(uri.toStdString())));
    QVariantList pluginPresets;
    for (const auto &entry : *own) {
        const auto map = entry.toMap();
        pluginPresets.append(QVariantMap{{"id", map.value("id")}, {"name", map.value("name")}});
    }
    out["builtIn"] = builtIn;
    out["pluginPresets"] = pluginPresets;
}

bool Backend::choosePreset(const QString &id, const QString &uri, const QVariantList &fields, ChosenPreset &out)
{
    // A starting point or the effect's own preset sets every control: the ones it
    // names to its values, every other one to the effect's default.
    const auto resolve = [&](const QString &name, const QMap<QString, double> &values) {
        out = {};
        out.name = name;
        for (const auto &entry : fields) {
            const auto field = entry.toMap();
            const auto symbol = field.value("legacySymbol").toString();
            const auto given = values.constFind(symbol);
            out.controls.insert(symbol, fitValue(field, given != values.cend() ? given.value() : field.value("default").toDouble()));
        }
        return true;
    };
    if (id.startsWith(QLatin1String("builtin:"))) {
        const auto *preset = builtInPreset(id);
        if (!preset || !preset->uris.contains(uri)) { setError(tr("This starting point is not for this effect.")); return false; }
        return resolve(preset->name, preset->controls);
    }
    if (id.startsWith(QLatin1String("lv2preset:"))) {
        QVariantMap scratch;
        listPresets(uri, fields, scratch);
        for (const auto &entry : m_ownPluginPresets.value(uri)) {
            const auto map = entry.toMap();
            if (map.value("id").toString() != id) continue;
            QMap<QString, double> values;
            const auto controls = map.value("controls").toMap();
            for (auto it = controls.cbegin(); it != controls.cend(); ++it) values.insert(it.key(), it.value().toDouble());
            return resolve(map.value("name").toString(), values);
        }
        setError(tr("This effect no longer has that preset.")); return false;
    }
    if (const auto failure = m_pluginPresets.load(); !failure.isEmpty()) { setError(failure); return false; }
    const auto &entries = m_pluginPresets.entries();
    const auto found = std::find_if(entries.cbegin(), entries.cend(), [&](const auto &entry) { return entry.id == id; });
    if (found == entries.cend() || found->uri != uri) {
        setError(tr("This preset is missing or belongs to another plugin. Reload the library.")); return false;
    }
    if (found->controls.size() != fields.size()) {
        setError(tr("This preset's parameters do not match the installed plugin version.")); return false;
    }
    out = {};
    out.name = found->name; out.controls = found->controls; out.parameterIds = found->parameterIds; out.states = found->states;
    return true;
}

QVariantMap Backend::pluginPresetContext(int track, int slot)
{
    QVariantMap out{{"available", false}, {"track", track}, {"slot", slot}, {"stamp", automationStamp()}};
    const auto reject = [&](const QString &reason) { out["reason"] = reason; return out; };
    if (m_shuttingDown || m_loading || m_scanning || m_recording || automationWriting()
        || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current edit before using parameter presets."));
    if (track < 0 && track != kMasterTarget) return reject(tr("Choose a track or the master bus."));
    const auto target = chainTarget(track);
    if (!target.inserts || slot < 0 || std::size_t(slot) >= target.inserts->size())
        return reject(tr("The chosen effect no longer exists. Reopen presets from the Track Effects panel."));
    const auto &insert = (*target.inserts)[std::size_t(slot)];
    QString reason;
    const auto fields = presetFields(insert, reason);
    if (fields.isEmpty()) return reject(reason);
    out["name"] = QString::fromStdString(m_lv2->find(insert.uri)->name); out["uri"] = QString::fromStdString(insert.uri);
    if (const auto failure = m_pluginPresets.load(); !failure.isEmpty()) return reject(failure);
    QVariantList presets;
    for (const auto &entry : m_pluginPresets.entries()) if (entry.uri == out.value("uri").toString())
        presets.append(QVariantMap{{"id", entry.id}, {"name", entry.name}, {"count", entry.controls.size()},
                                   {"complete", !entry.states.isEmpty()}});
    listPresets(out.value("uri").toString(), fields, out);
    const auto live = m_chains.find(target.key);
    out["completeSettings"] = !insert.bypass && live != m_chains.end() && live->second
        && live->second->hasOpaqueState(std::size_t(slot));
    out["fields"] = fields; out["presets"] = presets; out["available"] = true;
    return out;
}

QVariantMap Backend::clipEffectPresetContext(int track, int clip, int slot)
{
    QVariantMap out{{"available", false}, {"track", track}, {"clip", clip}, {"slot", slot}};
    const auto reject = [&](const QString &reason) { out["reason"] = reason; return out; };
    const auto target = clipEffectsTarget(track, clip);
    if (!target.value("valid").toBool()) return reject(target.value("reason").toString());
    out["stamp"] = target.value("stamp");
    const auto *lane = m_project.track(session::TrackId(track));
    if (!lane || clip < 0 || std::size_t(clip) >= lane->clips.size() || !lane->clips[std::size_t(clip)].rack
        || slot < 0 || std::size_t(slot) >= lane->clips[std::size_t(clip)].rack->inserts.size())
        return reject(tr("That clip effect no longer exists."));
    const auto &insert = lane->clips[std::size_t(clip)].rack->inserts[std::size_t(slot)];
    QString reason;
    const auto fields = presetFields(insert, reason);
    if (fields.isEmpty()) return reject(reason);
    out["name"] = QString::fromStdString(m_lv2->find(insert.uri)->name); out["uri"] = QString::fromStdString(insert.uri);
    if (const auto failure = m_pluginPresets.load(); !failure.isEmpty()) return reject(failure);
    QVariantList presets;
    for (const auto &entry : m_pluginPresets.entries())
        if (entry.uri == out.value("uri").toString() && entry.states.isEmpty())
            presets.append(QVariantMap{{"id", entry.id}, {"name", entry.name}, {"count", entry.controls.size()}, {"complete", false}});
    listPresets(out.value("uri").toString(), fields, out);
    out["fields"] = fields; out["presets"] = presets; out["available"] = true;
    return out;
}

bool Backend::applyClipEffectPreset(int track, int clip, int slot, const QString &id, const QString &stamp)
{
    const auto context = clipEffectPresetContext(track, clip, slot);
    if (!context.value("available").toBool()) { setError(context.value("reason").toString()); return false; }
    const auto fields = context.value("fields").toList();
    ChosenPreset chosen;
    if (!choosePreset(id, context.value("uri").toString(), fields, chosen)) return false;
    if (!chosen.states.isEmpty()) {
        setError(tr("This preset keeps the effect's own complete settings; use it on a track's effect instead.")); return false;
    }
    for (const auto &entry : fields) {
        const auto field = entry.toMap();
        const auto symbol = field.value(chosen.parameterIds ? "symbol" : "legacySymbol").toString();
        const auto value = chosen.controls.constFind(symbol);
        if (value == chosen.controls.cend() || !acceptsValue(field, value.value())) {
            setError(tr("The saved %1 value is incompatible with this plugin or project rate.").arg(field.value("name").toString())); return false;
        }
        if (field.value("automated").toBool() && value.value() != field.value("value").toDouble()) {
            setError(tr("%1 is automated. Edit or disable its automation before applying this preset.").arg(field.value("name").toString())); return false;
        }
    }
    return replaceClipRack(track, clip, stamp, "apply preset " + chosen.name.toStdString(),
        [&](std::vector<session::Insert> &inserts, QString &error) {
            if (slot < 0 || std::size_t(slot) >= inserts.size()) { error = tr("That clip effect no longer exists."); return false; }
            auto &insert = inserts[std::size_t(slot)];
            for (const auto &entry : fields) {
                const auto field = entry.toMap();
                if (field.value("automated").toBool()) continue;
                const auto symbol = field.value(chosen.parameterIds ? "symbol" : "legacySymbol").toString();
                insert.setControl(field.value("port").toUInt(), chosen.controls.value(symbol));
            }
            return true;
        });
}

QString Backend::savePluginPreset(int track, int slot, const QString &stamp, const QString &name)
{
    if (!automationStampCurrent(stamp)) { setError(tr("The project changed. Reload before saving this preset.")); return {}; }
    const auto context = pluginPresetContext(track, slot);
    if (!context.value("available").toBool()) { setError(context.value("reason").toString()); return {}; }
    QMap<QString, double> controls;
    for (const auto &entry : context.value("fields").toList()) {
        const auto field = entry.toMap(); controls.insert(field.value("symbol").toString(), field.value("value").toDouble());
    }
    QString id;
    if (const auto error = m_pluginPresets.add(name, context.value("uri").toString(), controls, id); !error.isEmpty()) {
        setError(error); return {};
    }
    setError({}); return id;
}

bool Backend::savePluginPresetWithState(int track, int slot, const QString &stamp, const QString &name)
{
    if (!automationStampCurrent(stamp)) { setError(tr("The project changed. Reload before saving this preset.")); return false; }
    const auto context = pluginPresetContext(track, slot);
    if (!context.value("available").toBool()) { setError(context.value("reason").toString()); return false; }
    const auto target = chainTarget(track);
    const auto& insert = (*target.inserts)[std::size_t(slot)];
    const auto live = m_chains.find(target.key);
    if (live == m_chains.end() || !live->second || insert.bypass || !live->second->hasOpaqueState(std::size_t(slot))) {
        setError(tr("This effect keeps all its settings in its parameters; save a parameter preset instead.")); return false;
    }
    const QString trimmed = name.trimmed();
    for (const auto& entry : m_pluginPresets.entries())
        if (entry.uri == context.value("uri").toString() && entry.name == trimmed) {
            setError(tr("A preset with this name already exists for this plugin. Choose another name.")); return false;
        }
    QMap<QString, double> controls;
    for (const auto &entry : context.value("fields").toList()) {
        const auto field = entry.toMap(); controls.insert(field.value("symbol").toString(), field.value("value").toDouble());
    }
    const auto key = target.key;
    const auto identity = insert.identity;
    const auto uri = context.value("uri").toString();
    setError({});
    requestDeliverySnapshot(key, false, [this] { return m_shuttingDown; },
        [this, key, identity, uri, trimmed, controls](DeliverySnapshot snapshot, QString error, bool cancelled) {
            const auto finish = [this](const QString& id, const QString& problem) {
                if (!problem.isEmpty()) setError(problem);
                emit pluginPresetSaved(id, problem);
            };
            if (cancelled) { finish({}, tr("Saving the preset was cancelled.")); return; }
            if (!error.isEmpty()) { finish({}, error); return; }
            const auto* rack = session::findEffectRack(snapshot.project, key);
            const session::Insert* captured = nullptr;
            if (rack) for (const auto& candidate : *rack) if (candidate.identity == identity) captured = &candidate;
            if (!captured || captured->instanceState.empty()) { finish({}, tr("The effect did not provide its settings. Try again.")); return; }
            QStringList states;
            for (const auto& state : captured->instanceState) {
                if (const auto problem = io::writePluginState(m_pluginPresets.stateRoot().toStdString(), state); !problem.empty()) {
                    finish({}, tr("Could not store the effect's settings: %1").arg(QString::fromStdString(problem))); return;
                }
                states.append(QString::fromStdString(state.id().toString()));
            }
            QString id;
            if (const auto problem = m_pluginPresets.add(trimmed, uri, controls, id, states); !problem.isEmpty()) { finish({}, problem); return; }
            finish(id, {});
        });
    return true;
}

bool Backend::deletePluginPreset(const QString &id)
{
    if (const auto error = m_pluginPresets.remove(id); !error.isEmpty()) { setError(error); return false; }
    setError({}); return true;
}

bool Backend::applyPluginPreset(int track, int slot, const QString &stamp, const QString &id)
{
    if (const auto deferred = deferAutomationEdit([this, track, slot, stamp, id] { return applyPluginPreset(track, slot, stamp, id); })) return *deferred;
    if (!automationStampCurrent(stamp)) { setError(tr("The project changed. Reload before applying this preset.")); return false; }
    const auto context = pluginPresetContext(track, slot);
    if (!context.value("available").toBool()) { setError(context.value("reason").toString()); return false; }
    const auto fields = context.value("fields").toList();
    ChosenPreset chosen;
    if (!choosePreset(id, context.value("uri").toString(), fields, chosen)) return false;
    const auto* found = &chosen;
    const auto target = chainTarget(track);
    const auto identity = (*target.inserts)[std::size_t(slot)].identity;
    if (auto deferred = deferPluginEdit([this, key = target.key, track = target.target, identity, id] {
        const int current = pluginSlot(key, identity);
        if (current < 0) { setError(tr("The effect changed before its preset could be applied.")); return false; }
        return applyPluginPreset(track, current, automationStamp(), id);
    })) return *deferred;
    auto proposed = *target.inserts;
    auto &insert = proposed[std::size_t(slot)];
    QSet<QString> identities;
    for (const auto &entry : fields) {
        const auto field = entry.toMap();
        const auto symbol = field.value(found->parameterIds ? "symbol" : "legacySymbol").toString();
        if (identities.contains(symbol)) { setError(tr("This older preset has ambiguous parameter names.")); return false; }
        identities.insert(symbol);
        const auto value = found->controls.constFind(symbol);
        if (value == found->controls.cend() || !acceptsValue(field, value.value())) {
            setError(tr("The saved %1 value is incompatible with this plugin or project rate.").arg(field.value("name").toString())); return false;
        }
        const auto* current = insert.control(field.value("port").toUInt());
        // Applying a parameter preset explicitly addresses every actual instance.
        // The representative value may match while the second mono instance differs.
        if (value.value() == field.value("value").toDouble()
            && (field.value("automated").toBool() || !(current && current->fromState))) continue;
        if (field.value("automated").toBool()) {
            setError(tr("%1 is automated. Edit or disable its automation before applying this preset.").arg(field.value("name").toString())); return false;
        }
        insert.setControl(field.value("port").toUInt(), value.value());
    }
    if (!found->states.isEmpty()) {
        // The effect's own complete settings; the saved parameters are already in them.
        const auto live = m_chains.find(target.key);
        const auto instances = live != m_chains.end() && live->second ? live->second->instanceCount(std::size_t(slot)) : 0;
        if (std::size_t(found->states.size()) != instances) {
            setError(tr("This preset was saved for a different channel layout of this effect.")); return false;
        }
        std::vector<session::PluginState> states;
        for (const auto& text : found->states) {
            const auto uuid = session::Uuid::parse(text.toStdString());
            std::string problem;
            auto state = uuid ? io::readPluginState(m_pluginPresets.stateRoot().toStdString(), *uuid, insert.uri,
                                                     session::PluginState::kProjectLimit, problem) : std::nullopt;
            if (!state) { setError(tr("This preset's effect settings are missing or damaged: %1").arg(QString::fromStdString(problem))); return false; }
            states.push_back(std::move(*state));
        }
        insert.instanceState = std::move(states);
        for (auto& control : insert.controls) if (!control.automationActive()) control.fromState = true;
    }
    if (proposed == *target.inserts) { setError({}); return true; }
    if (const auto error = session::validateElastic(m_project); !error.empty()) { setError(QString::fromStdString(error)); return false; }
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
    *target.inserts = std::move(proposed);
    if (auto old = m_chains.find(target.key); old != m_chains.end() && old->second)
        m_retiredChains.push_back(std::move(old->second));
    m_chains[target.key] = std::move(prepared);
    establishPluginHistory(target.key, m_chains[target.key]);
    m_chainSpecs[target.key] = *target.inserts;
    setError({}); commit("apply parameter preset " + found->name.toStdString(), target.track, before);
    if (m_pluginPortsTarget == track && m_pluginPortsSlot >= 0) requestPluginPorts(m_pluginPortsSlot, track);
    return true;
}
