// The frequency response of an equaliser or filter, for the curve drawn above its
// controls. Measured, not modelled: an impulse goes through a separate offline
// copy of the effect with the slot's current settings, and the FFT of what comes
// out is the response. That works for any linear effect, whatever its format,
// without knowing its maths. The copy is built on this thread (as Apply builds
// its chain), then warmed and run on a thread of its own; results are kept by
// the effect and its settings, so moving back to a setting already seen is
// immediate. Only the newest settings asked for while one runs are measured next.
//
// Bands (the handles on the curve) are found from the control names and units:
// a frequency control, and the gain and width controls whose names share the rest
// of its symbol ("p1_freq", "p1_level", "p1_q"; "freq2", "gain2", "q2").
#include "backend.h"
#include "pluginmetadata.h"
#include "render/master_render.h"
#include "spectral/fft.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <complex>

namespace {

constexpr int kPoints = 256;
constexpr double kLowHz = 20, kHighHz = 20000;
constexpr int kCached = 48;

std::uint32_t impulseFrames(std::uint32_t rate)
{
    // Long enough for a steep low cut's ring at 20 Hz: about 1.4 s at 48 kHz.
    std::uint32_t frames = 1u << 14;
    while (frames < rate * 1.3 && frames < (1u << 18)) frames <<= 1;
    return frames;
}

// The gain at each of the drawn frequencies, from an impulse response. Each point
// stands for the bins half-way to its neighbours and shows the one furthest from
// 0 dB, as an analyser does: a smooth curve is unchanged, and a notch a few hertz
// wide still shows its depth rather than being averaged away.
QVariantList measure(const std::vector<float>& response, std::uint32_t rate, QVariantList& frequencies)
{
    const auto n = std::uint32_t(response.size());
    omawave::spectral::Fft fft(n);
    std::vector<std::complex<float>> data(n);
    for (std::uint32_t i = 0; i < n; ++i) data[i] = response[i];
    fft.forward(data.data());
    const double nyquist = rate / 2.0, binHz = double(rate) / n;
    const double high = std::min(kHighHz, nyquist * 0.98);
    const double step = std::pow(high / kLowHz, 1.0 / (kPoints - 1));
    const auto binDb = [&](std::uint32_t b) { return 10 * std::log10(std::max(double(std::norm(data[b])), 1e-24)); };
    QVariantList db;
    frequencies.clear();
    for (int p = 0; p < kPoints; ++p) {
        const double f = kLowHz * std::pow(step, p);
        const double half = std::sqrt(step);
        auto from = std::uint32_t(std::ceil(f / half / binHz)), to = std::uint32_t(std::floor(f * half / binHz));
        const auto nearest = std::clamp<std::uint32_t>(std::uint32_t(std::lround(f / binHz)), 1, n / 2);
        from = std::clamp<std::uint32_t>(from, 1, n / 2); to = std::clamp<std::uint32_t>(to, 1, n / 2);
        double value = binDb(nearest);
        for (auto b = from; b <= to; ++b)
            if (const double v = binDb(b); std::abs(v) > std::abs(value)) value = v;
        frequencies.append(f);
        db.append(std::clamp(value, -120.0, 60.0));
    }
    return db;
}

enum class Role { None, Frequency, Gain, Width, Other };
Role roleOf(const QString& word);

// The words a symbol is made of: "p1_freq" → p, 1, freq; "freq2" → freq, 2.
// A short band name run into its role is split too: "hpfreq" → hp, freq;
// "lsgain" → ls, gain; "hsq" → hs, q.
QStringList words(const QString& symbol)
{
    static const QRegularExpression split(QStringLiteral("[a-z]+|[0-9]+"));
    static const QStringList suffixes{"frequency", "freq", "gain", "level", "bw", "res", "q"};
    QStringList out;
    auto it = split.globalMatch(symbol.toLower());
    while (it.hasNext()) {
        const auto word = it.next().captured();
        bool joined = false;
        // A role word on its own ("freq") is never split ("fre" + "q").
        for (const auto& suffix : roleOf(word) == Role::None ? suffixes : QStringList{}) {
            const auto prefix = word.size() - suffix.size();
            if (word != suffix && word.endsWith(suffix) && prefix >= 1 && prefix <= 3) {
                out << word.left(prefix) << suffix; joined = true; break;
            }
        }
        if (!joined) out << word;
    }
    return out;
}

Role roleOf(const QString& word)
{
    static const QStringList frequency{"f", "fc", "fr", "freq", "frequency", "cutoff", "hz"};
    static const QStringList gain{"g", "gain", "gn", "level", "lvl", "boost"};
    static const QStringList width{"q", "bw", "bandwidth", "width", "res", "reso", "resonance"};
    static const QStringList other{"enable", "enabled", "active", "on", "type", "mode", "ft", "fm", "sec", "slope", "solo", "mute"};
    if (frequency.contains(word)) return Role::Frequency;
    if (gain.contains(word)) return Role::Gain;
    if (width.contains(word)) return Role::Width;
    if (other.contains(word)) return Role::Other;
    return Role::None;
}

QString bandLabel(const QString& key, int index, bool zeroBased)
{
    static const QHash<QString, QString> names{
        {"ls", "L"}, {"lowshelf", "L"}, {"lsh", "L"}, {"low", "L"}, {"lo", "L"},
        {"hs", "H"}, {"highshelf", "H"}, {"hsh", "H"}, {"high", "H"}, {"hi", "H"},
        {"hp", "HP"}, {"hpf", "HP"}, {"hipass", "HP"}, {"highpass", "HP"}, {"lowcut", "HP"}, {"lc", "HP"},
        {"lp", "LP"}, {"lpf", "LP"}, {"lopass", "LP"}, {"lowpass", "LP"}, {"highcut", "LP"}, {"hc", "LP"}};
    const auto compact = QString(key).remove(QLatin1Char(' '));
    if (names.contains(compact)) return names.value(compact);
    static const QRegularExpression digits(QStringLiteral("(\\d+)"));
    if (const auto m = digits.match(compact); m.hasMatch()) return QString::number(m.captured(1).toInt() + (zeroBased ? 1 : 0));
    return QString::number(index + 1);
}

}  // namespace

QString Backend::pluginKind(int slot, int target)
{
    // One rule for both: the slot's effect, by the same category as the panel's.
    const auto chain = chainTarget(target);
    if (!chain.inserts || slot < 0 || std::size_t(slot) >= chain.inserts->size()) return QStringLiteral("other");
    return pluginKind(QString::fromStdString((*chain.inserts)[std::size_t(slot)].uri));
}

const std::vector<lv2::ControlPort>* Backend::responsePorts(const ChainTarget& target, int slot)
{
    const auto& insert = (*target.inserts)[std::size_t(slot)];
    const auto live = m_chains.find(target.key);
    const auto* identity = live != m_chains.end() && live->second ? live->second->insertIdentity(std::size_t(slot)) : nullptr;
    if (identity && *identity == insert.identity) return live->second->controls(std::size_t(slot));
    // The chain is being rebuilt (just added, moved or removed): the effect's
    // controls from a probe made once per effect.
    if (const auto it = m_responseProbePorts.find(insert.uri); it != m_responseProbePorts.end()) return &it->second;
    std::string error;
    auto probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error, insert.legacyClapControls,
        insert.instanceState.empty() ? nullptr : &insert.instanceState.front(),
        {insert.instanceState.size() == 2 ? 1u : kOutputChannels});
    if (!probe) return nullptr;
    return &(m_responseProbePorts[insert.uri] = probe->controls());
}

double Backend::responseValue(const ChainTarget& target, int slot, const lv2::ControlPort& port) const
{
    // What the project holds is current (a drag writes it every tick); else the
    // running instance's value, else the default.
    const auto& insert = (*target.inserts)[std::size_t(slot)];
    if (const auto* stored = insert.control(port.index)) return stored->value;
    const auto live = m_chains.find(target.key);
    const auto* identity = live != m_chains.end() && live->second ? live->second->insertIdentity(std::size_t(slot)) : nullptr;
    if (identity && *identity == insert.identity) {
        const double value = live->second->controlValue(std::size_t(slot), port.index);
        if (std::isfinite(value)) return value;
    }
    return port.defaultValue;
}

QVariantList Backend::responseBands(const ChainTarget& target, int slot)
{
    const auto& insert = (*target.inserts)[std::size_t(slot)];
    const auto* ports = responsePorts(target, slot);
    if (!ports) return {};
    const auto* plugin = m_lv2->lilvPlugin(insert.uri);
    struct Port { std::uint32_t index; double min, max, value; bool log; bool linear = false; };
    struct Band { QString key; std::optional<Port> frequency, gain, width; int enablePort = -1; bool enabled = true; };
    std::vector<Band> bands;
    const auto bandFor = [&](const QString& key) -> Band& {
        for (auto& b : bands) if (b.key == key) return b;
        bands.push_back({key, {}, {}, {}});
        return bands.back();
    };
    for (const auto& port : *ports) {
        if (port.output) continue;
        // A band's own switch ("sec1", "p1_active", "ls_enable"): 0 is off.
        {
            static const QStringList switches{"enable", "enabled", "active", "sec"};
            auto parts = words(QString::fromStdString(port.symbol));
            for (int i = 0; i < parts.size(); ++i)
                if (switches.contains(parts[i])) {
                    parts.removeAt(i);
                    auto& band = bandFor(parts.join(QLatin1Char(' ')));
                    if (band.enablePort < 0 && !parts.isEmpty()) {
                        band.enablePort = int(port.index);
                        band.enabled = responseValue(target, slot, port) != 0;
                    }
                    break;
                }
        }
        if (port.toggled || !(port.maximum > port.minimum)) continue;
        const auto presentation = pluginControlPresentation(m_lv2->raw(), plugin,
            plugin ? lilv_plugin_get_port_by_index(plugin, port.index) : nullptr, port.minimum, port.maximum);
        if (!presentation.value("choices").toList().isEmpty()) continue;
        const auto unit = presentation.value("unit").toString().toLower();
        auto parts = words(QString::fromStdString(port.symbol));
        Role role = Role::None;
        for (int i = 0; i < parts.size(); ++i)
            if (const auto r = roleOf(parts[i]); r != Role::None) { role = r; parts.removeAt(i); break; }
        if (role == Role::Other) continue;
        if (unit == QLatin1String("hz") || unit == QLatin1String("khz")) {
            if (role != Role::Frequency && role != Role::None) continue;     // e.g. a "width" in Hz
            role = Role::Frequency;
        } else if (unit == QLatin1String("db") && role == Role::None) {
            role = Role::Gain;
        }
        if (role == Role::None) continue;
        if (role == Role::Frequency && (port.minimum <= 0 || port.maximum < 40)) continue;   // not audio frequencies
        const double value = responseValue(target, slot, port);
        Port entry{port.index, port.minimum, port.maximum, std::isfinite(value) ? value : port.defaultValue,
                   role == Role::Frequency || presentation.value("logarithmic").toBool()};
        // A gain without dB that spans 1 (unity) from above zero is a factor:
        // 0.016..64 is −36..+36 dB.
        entry.linear = role == Role::Gain && unit != QLatin1String("db") && port.minimum > 0
            && port.minimum < 1 && port.maximum > 1;
        auto& band = bandFor(parts.join(QLatin1Char(' ')));
        auto& slotFor = role == Role::Frequency ? band.frequency : role == Role::Gain ? band.gain : band.width;
        if (!slotFor) slotFor = entry;
    }
    // Only bands with a frequency are handles; in frequency order.
    bands.erase(std::remove_if(bands.begin(), bands.end(), [](const Band& b) { return !b.frequency; }), bands.end());
    bool zeroBased = false;
    for (const auto& b : bands) if (words(b.key).contains(QStringLiteral("0"))) zeroBased = true;
    QVariantList out;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const auto& b = bands[i];
        const auto port = [](const std::optional<Port>& p) { return p ? int(p->index) : -1; };
        out.append(QVariantMap{{"label", bandLabel(b.key, int(i), zeroBased)}, {"key", b.key},
            {"freqPort", port(b.frequency)}, {"gainPort", port(b.gain)}, {"qPort", port(b.width)},
            {"freqMin", b.frequency->min}, {"freqMax", b.frequency->max}, {"freq", b.frequency->value},
            {"gainMin", b.gain ? b.gain->min : 0.0}, {"gainMax", b.gain ? b.gain->max : 0.0}, {"gain", b.gain ? b.gain->value : 0.0},
            {"qMin", b.width ? b.width->min : 0.0}, {"qMax", b.width ? b.width->max : 0.0}, {"q", b.width ? b.width->value : 0.0},
            {"qLog", b.width ? b.width->log : false}, {"gainLinear", b.gain ? b.gain->linear : false},
            {"enablePort", b.enablePort}, {"enabled", b.enabled}});
    }
    return out;
}

QVariantMap Backend::pluginResponse(int slot, int target)
{
    const auto chain = chainTarget(target);
    if (!chain.inserts || slot < 0 || std::size_t(slot) >= chain.inserts->size())
        return {{"valid", false}, {"pending", false}, {"reason", tr("There is no such effect.")}};
    const auto rate = m_project.sampleRate() ? m_project.sampleRate() : 48000u;
    auto insert = (*chain.inserts)[std::size_t(slot)];
    const bool bypassed = insert.bypass;
    insert.bypass = false;
    insert.forkIdentity();
    // The settings as they are now, including a drag still going on.
    if (!m_lv2->scanned()) m_lv2->scan();
    const auto* ports = responsePorts(chain, slot);
    QString key = QString::fromStdString(insert.uri) + QLatin1Char('|') + QString::number(rate);
    if (ports) {
        for (const auto& port : *ports) {
            if (port.output) continue;
            const double value = responseValue(chain, slot, port);
            if (!std::isfinite(value)) continue;
            insert.setControl(port.index, value);
            key += QLatin1Char('|') + QString::number(value, 'g', 9);
        }
    } else {
        for (const auto& c : insert.controls) key += QLatin1Char('|') + QString::number(c.value, 'g', 9);
    }
    for (const auto& state : insert.instanceState) {
        if (!state) continue;
        const auto bytes = state.processor();
        key += QLatin1Char('|') + QString::number(qHash(QByteArrayView(bytes.data(), qsizetype(bytes.size()))));
    }

    const auto decorate = [&](QVariantMap result, bool pending) {
        result["pending"] = pending;
        result["bypassed"] = bypassed;
        result["kind"] = pluginKind(slot, target);
        result["bands"] = responseBands(chain, slot);
        return result;
    };
    if (m_responses.contains(key)) return decorate(m_responses.value(key), false);

    // Measure in the background; the newest settings asked for win.
    m_responseWanted = key;
    if (m_responseRunning.isEmpty() && !m_shuttingDown) {
        auto features = std::make_shared<lv2::HostFeatures>(m_urids, rate, lv2::kMaxBlock);
        QString error;
        std::shared_ptr<lv2::Chain> offline = buildOfflineChain({insert}, *features, error);
        if (!offline) {
            QVariantMap failed{{"valid", false}, {"reason", error.isEmpty() ? tr("The effect could not be measured.") : error}};
            m_responses.insert(key, failed);
            m_responseOrder << key;
            return decorate(failed, false);
        }
        if (m_responseThread.joinable()) m_responseThread.join();
        m_responseRunning = key;
        m_responseCancel.store(false, std::memory_order_relaxed);
        m_responseThread = std::thread([this, key, rate, offline, features] {
            // A quarter of a second of silence first: some effects fade in after
            // they start, and would swallow an impulse at the very first frame.
            const auto frames = impulseFrames(rate), preroll = rate / 4;
            session::AudioBuffer impulse(kOutputChannels, preroll + frames);
            for (std::uint32_t c = 0; c < kOutputChannels; ++c) impulse.plane(c)[preroll] = 1.0f;
            QVariantMap result;
            if (!offline->warmUp(&m_responseCancel)) {
                result = {{"valid", false}, {"reason", tr("The effect could not start.")}};
            } else {
                auto wet = render::renderBus(std::move(impulse), 1.0f, offline.get(), &m_responseCancel);
                if (!wet.ok || wet.audio.frames() < preroll + frames) {
                    result = {{"valid", false}, {"reason", wet.error.empty() ? tr("The effect could not be measured.") : QString::fromStdString(wet.error)}};
                } else {
                    const auto plane = wet.audio.plane(0);
                    std::vector<float> response(plane.begin() + preroll, plane.begin() + preroll + frames);
                    QVariantList frequencies;
                    const auto db = measure(response, rate, frequencies);
                    result = {{"valid", true}, {"frequencies", frequencies}, {"db", db}};
                }
            }
            QMetaObject::invokeMethod(this, [this, key, result, offline] {
                if (m_responseThread.joinable()) m_responseThread.join();
                m_responseRunning.clear();
                if (m_shuttingDown) return;
                m_responses.insert(key, result);
                m_responseOrder.removeAll(key);
                m_responseOrder << key;
                while (m_responseOrder.size() > kCached) m_responses.remove(m_responseOrder.takeFirst());
                ++m_pluginResponseRevision;
                emit pluginResponseChanged();
            }, Qt::QueuedConnection);
        });
    }
    // Meanwhile, the last measurement of this effect (any settings), if there is one.
    const auto prefix = QString::fromStdString(insert.uri) + QLatin1Char('|');
    for (auto it = m_responseOrder.crbegin(); it != m_responseOrder.crend(); ++it)
        if (it->startsWith(prefix) && m_responses.value(*it).value("valid").toBool())
            return decorate(m_responses.value(*it), true);
    return decorate({{"valid", false}, {"reason", tr("Measuring…")}}, true);
}
