// The Sound panel's fixes (a voice's clean-up and clarity, the width of effects
// and ambience): each is one effect on the track, driven through a single amount. Nothing is stored beside the effects themselves. The panel
// reads its sliders back from the effects' controls, so a change made in Track
// Effects simply shows here, and nothing can drift out of step.
//
// Like Quick Fixes, a fix drives the first effect of its kind on the track. One
// that is not there yet is added in a fixed order (low cut, hum, noise, "s",
// even out, presence), next to the fixes already present. Turning a fix off
// bypasses its effect and keeps its settings, so turning it on again restores
// them. Every call that changes anything is one Undo step.
#include "backend.h"
#include "session/elastic.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace {

using Controls = std::map<std::string, double>;     // symbol → raw port value

double toDb(double linear) { return 20 * std::log10(linear); }
double fromDb(double db) { return std::pow(10, db / 20); }

struct Fix {
    const char *id, *title, *uri, *pluginName;
    const char *primary;        // the control the amount is read back from
    double defaultAmount;
    bool needs48k = false;      // the live denoiser runs at 48 kHz only
    bool hasMains = false;      // hum: a 50 or 60 Hz choice instead of a slider
    // The controls for an amount (0..1) and a mains frequency.
    std::function<Controls(double amount, double mains)> controls;
    // The amount shown for the primary control's raw value.
    std::function<double(double raw)> amount;
    const char* kinds = "voice";  // which kinds of sound show it
    int scale = 100;              // the slider shows amount × scale, in %
};

const std::vector<Fix>& fixes()
{
    static const std::vector<Fix> list{
        {"rumble", "Rumble and pops", "urn:omaaudio:lowcut", "OmaAudio Low Cut", "frequency", 0.3, false, false,
            [](double a, double) { return Controls{{"frequency", 40 * std::pow(4.0, a)}}; },
            [](double raw) { return std::log(raw / 40) / std::log(4.0); }},
        {"hum", "Mains hum", "urn:omaaudio:hum", "OmaAudio Hum Removal", "", 1.0, false, true,
            [](double, double mains) { return Controls{{"mains", mains}, {"harmonics", 4}}; },
            [](double) { return 1.0; }},
        {"noise", "Background noise", "urn:omaaudio:denoise", "OmaAudio Voice Cleanup", "amount", 0.45, true, false,
            [](double a, double) { return Controls{{"amount", 100 * a}}; },
            [](double raw) { return raw / 100; }},
        {"sibilance", "Harsh “s”", "http://calf.sourceforge.net/plugins/Deesser", "Calf Deesser", "threshold", 0.35, false, false,
            [](double a, double) { return Controls{{"bypass", 0}, {"threshold", fromDb(-6 - 30 * a)}, {"ratio", 2 + 4 * a},
                {"laxity", 15}, {"f1_freq", 6000}, {"f2_freq", 4500}, {"makeup", 1}}; },
            [](double raw) { return (-6 - toDb(raw)) / 30; }},
        {"evenout", "Even out", "http://calf.sourceforge.net/plugins/Compressor", "Calf Compressor", "threshold", 0.5, false, false,
            [](double a, double) {
                const double threshold = -10 - 20 * a, ratio = 1.5 + 3.5 * a;
                // Win back about half of what the compressor takes off the peaks;
                // Loudness sets the final level with the fader.
                const double makeup = -threshold * (1 - 1 / ratio) * 0.5;
                return Controls{{"bypass", 0}, {"threshold", fromDb(threshold)}, {"ratio", ratio}, {"attack", 15},
                    {"release", 150}, {"makeup", fromDb(makeup)}, {"mix", 1}};
            },
            [](double raw) { return (-10 - toDb(raw)) / 20; }},
        {"presence", "Presence", "http://calf.sourceforge.net/plugins/Exciter", "Calf Exciter", "amount", 0.3, false, false,
            [](double a, double) { return Controls{{"bypass", 0}, {"freq", 3000}, {"drive", 3}, {"blend", 0}, {"amount", 0.4 * a}}; },
            [](double raw) { return raw / 0.4; }},
        // Effects and ambience: 0.5 leaves the width alone (100 %).
        {"width", "Width", "urn:omaaudio:width", "OmaAudio Width", "width", 0.5, false, false,
            [](double a, double) { return Controls{{"width", 200 * a}}; },
            [](double raw) { return raw / 200; }, "effects ambience", 200},
    };
    return list;
}

const Fix* fix(const QString& id)
{
    for (const auto& f : fixes()) if (id == QLatin1String(f.id)) return &f;
    return nullptr;
}

int rankOf(const std::string& uri)
{
    const auto& list = fixes();
    for (std::size_t i = 0; i < list.size(); ++i) if (uri == list[i].uri) return int(i);
    return -1;
}

// Where a new fix goes: after the nearest earlier fix already on the track, else
// before the nearest later one, else first in the chain.
std::size_t placeFor(const std::vector<omawave::session::Insert>& inserts, int rank)
{
    std::optional<std::size_t> after, before;
    for (std::size_t i = 0; i < inserts.size(); ++i) {
        const int r = rankOf(inserts[i].uri);
        if (r < 0) continue;
        if (r < rank) after = i;
        else if (r > rank && !before) before = i;
    }
    return after ? *after + 1 : before ? *before : 0;
}

QString soundKind(omawave::session::TrackRole role)
{
    using omawave::session::TrackRole;
    switch (role) {
    case TrackRole::Dialogue: case TrackRole::Host: case TrackRole::Guest: return QStringLiteral("voice");
    case TrackRole::Music: return QStringLiteral("music");
    case TrackRole::Sfx: case TrackRole::Foley: return QStringLiteral("effects");
    case TrackRole::Ambience: return QStringLiteral("ambience");
    default: return {};
    }
}

}  // namespace

QVariantMap Backend::soundSettings(int track)
{
    QVariantMap out{{"available", false}, {"stamp", automationStamp()}, {"fixes", QVariantList{}}};
    const auto reject = [&](const QString& reason) { out["reason"] = reason; return out; };
    const auto* t = m_project.track(trackArg(track));
    if (!t) return reject(tr("Click a track to set up its sound."));
    out["track"] = int(t->id);
    out["trackName"] = QString::fromStdString(t->name);
    out["role"] = QString::fromLatin1(session::roleName(t->role).data(), qsizetype(session::roleName(t->role).size()));
    const auto kind = soundKind(t->role);
    out["kind"] = kind;
    out["gainDb"] = dbFromLinear(t->gain);
    out["sampleRate"] = int(m_project.sampleRate());
    if (kind.isEmpty()) return reject(tr("Buses and the master have no Sound settings. Use Track Effects."));
    out["available"] = true;
    const bool catalogue = !m_scanning && m_lv2->scanned();
    if (!catalogue) out["reason"] = tr("Read the installed plugin catalogue to check availability.");

    QVariantList list;
    for (const auto& f : fixes()) {
        if (!QString::fromLatin1(f.kinds).split(QLatin1Char(' ')).contains(kind)) continue;
        QVariantMap entry{{"id", QString::fromLatin1(f.id)}, {"title", tr(f.title)}, {"plugin", QString::fromLatin1(f.pluginName)},
            {"live", true}, {"on", false}, {"amount", f.defaultAmount}, {"available", false}, {"slot", -1},
            {"choice", f.hasMains}, {"scale", f.scale}};
        if (f.hasMains) entry["mains"] = 50;
        const auto unavailable = [&](const QString& reason) { entry["reason"] = reason; list.append(entry); };
        if (f.needs48k && m_project.sampleRate() != 48000) {
            entry["live"] = false;
            unavailable(tr("Live noise reduction needs a 48 kHz project. Select the audio and apply Voice Cleanup instead."));
            continue;
        }
        if (!catalogue) { unavailable(out["reason"].toString()); continue; }
        const auto* info = m_lv2->find(f.uri);
        if (!info || !info->usable || !(info->fits(kOutputChannels) || info->fits(1))) {
            unavailable(info ? (info->usable ? tr("This plugin does not support the track’s channel layout.") : QString::fromStdString(info->unusableReason))
                : tr("Needs the optional %1 plugin.").arg(QString::fromLatin1(f.pluginName)));
            continue;
        }
        std::string error;
        auto probe = host::createInstance(*m_lv2, f.uri, hostFeatures(), error);
        if (!probe) { unavailable(QString::fromStdString(error)); continue; }
        const auto port = [&](const char* symbol) -> const host::ControlPort* {
            const auto found = std::find_if(probe->controls().begin(), probe->controls().end(),
                [&](const auto& p) { return !p.output && p.symbol == symbol; });
            return found == probe->controls().end() ? nullptr : &*found;
        };
        bool compatible = true;
        for (const auto& [symbol, value] : f.controls(f.defaultAmount, 50)) {
            const auto* p = port(symbol.c_str());
            compatible = compatible && p && std::isfinite(p->minimum) && std::isfinite(p->maximum) && p->maximum > p->minimum;
        }
        if (!compatible) { unavailable(tr("The installed plugin has incompatible controls. Use Track Effects.")); continue; }
        entry["available"] = true;
        const auto at = std::find_if(t->inserts.begin(), t->inserts.end(), [&](const auto& i) { return i.uri == f.uri; });
        if (at != t->inserts.end()) {
            const auto& insert = *at;
            entry["slot"] = int(at - t->inserts.begin());
            entry["on"] = !insert.bypass;
            const auto value = [&](const char* symbol) {
                const auto* p = port(symbol);
                const auto* stored = insert.control(p->index);
                return stored ? stored->value : p->defaultValue;
            };
            if (*f.primary) {
                const double raw = value(f.primary);
                entry["amount"] = std::isfinite(raw) && raw > 0 ? std::clamp(f.amount(raw), 0.0, 1.0) : 0.0;
            }
            if (f.hasMains) entry["mains"] = value("mains") >= 55 ? 60 : 50;
            bool automated = false;
            for (const auto& [symbol, unused] : f.controls(f.defaultAmount, 50)) {
                const auto* stored = insert.control(port(symbol.c_str())->index);
                automated = automated || (stored && stored->automationActive());
            }
            entry["automated"] = automated;
        }
        list.append(entry);
    }
    out["fixes"] = list;
    return out;
}

bool Backend::applySound(int track, const QString& stamp, const QVariantMap& changes)
{
    if (const auto deferred = deferAutomationEdit([this, track, stamp, changes] { return applySound(track, stamp, changes); })) return *deferred;
    if (m_shuttingDown || m_recording || automationWriting() || m_loading || m_scanning
        || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current edit before changing the sound.")); return false;
    }
    if (!automationStampCurrent(stamp)) {
        setError(tr("The track changed. Reload the Sound settings before changing them.")); return false;
    }
    const auto settings = soundSettings(track);
    if (!settings.value("available").toBool()) { setError(settings.value("reason").toString()); return false; }
    if (changes.isEmpty()) { setError(tr("Choose a fix to change.")); return false; }
    QMap<QString, QVariantMap> current;
    for (const auto& entry : settings.value("fixes").toList()) current.insert(entry.toMap().value("id").toString(), entry.toMap());
    auto* t = m_project.track(static_cast<session::TrackId>(settings.value("track").toInt()));
    auto proposed = t->inserts;
    QStringList changed;

    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const auto* f = fix(it.key());
        if (!f || !current.contains(it.key())) { setError(tr("Unknown fix: %1.").arg(it.key())); return false; }
        const auto now = current.value(it.key());
        const auto change = it.value().toMap();
        for (auto key = change.cbegin(); key != change.cend(); ++key)
            if (key.key() != "on" && key.key() != "amount" && key.key() != "mains") {
                setError(tr("%1 has no setting called %2.").arg(now.value("title").toString(), key.key())); return false;
            }
        const bool on = change.contains("on") ? change.value("on").toBool() : now.value("on").toBool();
        if (change.contains("on") && change.value("on").metaType().id() != QMetaType::Bool) {
            setError(tr("Turn %1 on or off with true or false.").arg(now.value("title").toString())); return false;
        }
        bool valid = true;
        const double amount = change.contains("amount") ? change.value("amount").toDouble(&valid) : now.value("amount").toDouble();
        if (!valid || !std::isfinite(amount) || amount < 0 || amount > 1 || (!*f->primary && change.contains("amount"))) {
            setError(*f->primary ? tr("Set %1 from 0 to 1.").arg(now.value("title").toString())
                                 : tr("%1 has no amount; choose 50 or 60 Hz.").arg(now.value("title").toString()));
            return false;
        }
        const double mains = change.contains("mains") ? change.value("mains").toDouble(&valid) : now.value("mains").toDouble();
        if (change.contains("mains") && (!f->hasMains || !valid || (mains != 50 && mains != 60))) {
            setError(f->hasMains ? tr("Choose 50 or 60 Hz for %1.").arg(now.value("title").toString())
                                 : tr("Only Mains hum has a 50 or 60 Hz choice."));
            return false;
        }
        if (!now.value("available").toBool()) {
            if (!on && now.value("slot").toInt() < 0) continue;     // nothing there to turn off
            setError(now.value("reason").toString()); return false;
        }
        if (now.value("automated").toBool() && (change.contains("amount") || change.contains("mains") || on)) {
            setError(tr("%1 is automated. Edit or disable its automation in Track Effects first.").arg(now.value("title").toString()));
            return false;
        }
        // Slots in `current` refer to the track's chain before this call;
        // find the effect again in the proposed one.
        auto at = std::find_if(proposed.begin(), proposed.end(), [&](const auto& i) { return i.uri == f->uri; });
        if (at == proposed.end()) {
            if (!on) continue;
            if (m_pluginLibrary.disabled().contains(QString::fromLatin1(f->uri))) {
                setError(tr("%1 is disabled for new effects. Enable it in the Plug-in Manager first.").arg(QString::fromLatin1(f->pluginName)));
                return false;
            }
            session::Insert insert; insert.uri = f->uri;
            at = proposed.insert(proposed.begin() + std::ptrdiff_t(placeFor(proposed, rankOf(f->uri))), std::move(insert));
        }
        auto& insert = *at;
        if (on || change.contains("amount") || change.contains("mains")) {
            std::string error;
            auto probe = host::createInstance(*m_lv2, f->uri, hostFeatures(), error);
            if (!probe) { setError(QString::fromStdString(error)); return false; }
            for (const auto& [symbol, value] : f->controls(amount, mains)) {
                const auto p = std::find_if(probe->controls().begin(), probe->controls().end(),
                    [&](const auto& c) { return !c.output && c.symbol == symbol; });
                double raw = p->clamp(value);
                if (p->integer || p->toggled) raw = std::round(raw);
                const auto* stored = insert.control(p->index);
                if (!stored ? raw != p->defaultValue : stored->value != raw) insert.setControl(p->index, raw);
            }
        }
        insert.bypass = !on;
        changed << now.value("title").toString();
    }
    if (proposed == t->inserts) { setError({}); return true; }
    if (const auto error = session::validateElastic(m_project); !error.empty()) {
        setError(QString::fromStdString(error)); return false;
    }

    // Warm the whole replacement before touching the model: a failure leaves
    // the running chain and the Undo history as they were.
    auto prepared = std::make_unique<lv2::Chain>(kOutputChannels, lv2::kMaxBlock);
    for (const auto& effect : proposed) {
        std::string error;
        if (!prepared->append(*m_lv2, hostFeatures(), effect, error)) {
            if (!effect.bypass) { setError(QString::fromStdString(error)); return false; }
            prepared->appendMissing(effect.identity);
        }
    }
    if (!prepared->warmUp(nullptr)) { setError(tr("Effect warm-up failed.")); return false; }
    const auto before = captureEditState();
    setError({});
    // Keep the Track Effects panel on the effect it was showing.
    if (m_pluginPortsTarget == int(t->id) && m_pluginPortsSlot >= 0 && std::size_t(m_pluginPortsSlot) < t->inserts.size()) {
        const auto& shown = t->inserts[std::size_t(m_pluginPortsSlot)].identity;
        for (std::size_t i = 0; i < proposed.size(); ++i)
            if (proposed[i].identity == shown && int(i) != m_pluginPortsSlot) { m_pluginPortsSlot = int(i); emit pluginPortsChanged(); break; }
    }
    t->inserts = std::move(proposed);
    const auto key = session::EffectOwner::track(t->id);
    if (auto old = m_chains.find(key); old != m_chains.end() && old->second)
        m_retiredChains.push_back(std::move(old->second));
    m_chains[key] = std::move(prepared); m_chainSpecs[key] = t->inserts;
    commit(changed.size() == 1 ? "adjust " + changed.first().toLower().toStdString() : std::string("adjust sound"), t->id, before);
    if (m_pluginPortsTarget == int(t->id) && m_pluginPortsSlot >= 0) requestPluginPorts(m_pluginPortsSlot, int(t->id));
    return true;
}

QVariantMap Backend::soundChoices(int track) const
{
    QVariantMap out;
    if (const auto found = m_soundChoices.find(session::TrackId(track)); found != m_soundChoices.end())
        for (const auto& [key, value] : found->second) out.insert(QString::fromStdString(key), value);
    return out;
}

void Backend::setSoundChoice(int track, const QString &key, double value)
{
    const auto* t = m_project.track(session::TrackId(track));
    if (!t || key.isEmpty() || !std::isfinite(value)) return;
    auto& choices = m_soundChoices[t->id];
    const auto name = key.toStdString();
    if (const auto found = choices.find(name); found != choices.end() && found->second == value) return;
    choices[name] = value;
    // A change to save, like the selection, but not an edit: no new revision, so
    // nothing measured or being analysed goes stale.
    armAutosave();
    if (!m_dirty) { m_dirty = true; emit timelineChanged(); }
}

// A Sound fix as a separate effect at a given amount, for a recipe step: the same
// settings the Sound tab would give it.
std::optional<omawave::session::Insert> Backend::soundFixInsert(const QString& id, double amount, double mains, QString& error)
{
    const auto* f = fix(id);
    if (!f) { error = tr("Unknown fix: %1.").arg(id); return std::nullopt; }
    std::map<std::string, double> controls;
    for (const auto& [symbol, value] : f->controls(std::clamp(amount, 0.0, 1.0), mains)) controls[symbol] = value;
    return recipeInsert(QString::fromLatin1(f->uri), controls, error);
}
