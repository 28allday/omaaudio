#include "backend.h"
#include "render/analysis.h"
#include "session/generate.h"
#include "session/render.h"
#include "session/edits.h"
#include "engine/loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Analysis panels (amplitude statistics, frequency analysis, phase), generated
// tones and noise, and loudness matching for clips. Analysis reads what the clips
// play — their effects, gain, fades and envelopes — before track processing.

namespace {
constexpr session::FramePos kMaxAnalysisSeconds = 20 * 60;

// One drawing curve per channel: `points` log-spaced from 20 Hz to Nyquist, each
// the loudest bin in its band (so narrow tones are kept), or interpolated between
// the neighbouring bins where a band is narrower than one bin.
QVariantList logCurves(const render::Spectrum& spectrum, int points)
{
    QVariantList curves;
    const auto rate = spectrum.sampleRate;
    const double nyquist = rate / 2.0;
    for (const auto& db : spectrum.magnitudeDb) {
        QVariantList curve;
        for (int p = 0; p < points; ++p) {
            const double low = 20 * std::pow(nyquist / 20, double(p) / points);
            const double high = 20 * std::pow(nyquist / 20, double(p + 1) / points);
            const double centre = std::sqrt(low * high);
            const auto first = std::size_t(std::ceil(low * spectrum.fftSize / rate));
            const auto last = std::min(std::size_t(std::floor(high * spectrum.fftSize / rate)), db.size() - 1);
            double value = -200;
            if (first <= last) {
                for (auto bin = first; bin <= last; ++bin) value = std::max(value, double(db[bin]));
            } else {
                const double position = centre * spectrum.fftSize / rate;
                const auto below = std::min(std::size_t(position), db.size() - 2);
                const double t = std::clamp(position - double(below), 0.0, 1.0);
                value = db[below] + (db[below + 1] - db[below]) * t;
            }
            curve.append(QVariantMap{{"frequency", centre}, {"db", value}});
        }
        curves.append(QVariant(curve));
    }
    return curves;
}
}

// The audio an analysis panel measures: the selection on its track, or the
// focused track's whole length. Bounded so a panel never holds an hour in memory.
static QString analysisRange(Backend& backend, const session::Project& project, session::TrackId focused,
                             const session::Selection& selection, session::TrackId& track,
                             session::FramePos& start, session::FramePos& end)
{
    track = selection.track != session::kInvalidTrack && !selection.empty() ? selection.track : focused;
    const auto* lane = project.track(track);
    if (!lane || lane->clips.empty()) return QObject::tr("Choose a track with audio to analyse.");
    if (!selection.empty() && selection.track == track) { start = selection.start; end = selection.end; }
    else { start = 0; end = lane->length(); }
    if (end <= start) return QObject::tr("Choose a range of audio to analyse.");
    if (end - start > kMaxAnalysisSeconds * session::FramePos(project.sampleRate()))
        return QObject::tr("Select up to 20 minutes to analyse at once.");
    (void)backend;
    return {};
}

QVariantMap Backend::analyseAudio(const QString& kind, int fftSize, const QString& window)
{
    const auto fail = [](const QString& reason) { return QVariantMap{{"ok", false}, {"reason", reason}}; };
    const auto heard = heardProject();
    session::TrackId track = session::kInvalidTrack;
    session::FramePos start = 0, end = 0;
    if (const auto reason = analysisRange(*this, heard.project, m_focused, m_selection, track, start, end); !reason.isEmpty())
        return fail(reason);
    if (!heard.missing.empty()) return fail(tr("Clip effects are still being prepared. Try again in a moment."));
    const auto* lane = heard.project.track(track);
    std::uint32_t channels = 1;
    for (const auto& clip : lane->clips)
        if (const auto* source = heard.project.source(clip.source)) channels = std::max(channels, source->audio.channels());
    session::AudioBuffer audio;
    try {
        audio = session::renderClipRange(heard.project, lane->clips, start, end, std::min<std::uint32_t>(channels, 8));
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
    const auto rate = heard.project.sampleRate();
    QVariantMap out{{"ok", true}, {"track", int(track)}, {"start", qint64(start)}, {"end", qint64(end)},
                    {"trackName", QString::fromStdString(lane->name)}, {"channels", int(audio.channels())}};
    if (kind == QLatin1String("statistics")) {
        const auto stats = render::amplitudeStatistics(audio, 0, audio.frames(), rate);
        if (!stats.ok) return fail(QString::fromStdString(stats.error));
        QVariantList perChannel;
        for (const auto& c : stats.channels)
            perChannel.append(QVariantMap{{"peakDb", c.peakDb}, {"rmsDb", c.rmsDb}, {"minimum", c.minimum},
                {"maximum", c.maximum}, {"dcOffsetPercent", c.dcOffset * 100}, {"clipped", qulonglong(c.clipped)},
                {"clippedRuns", qulonglong(c.clippedRuns)}});
        out.insert({{"peakDb", stats.peakDb}, {"rmsDb", stats.rmsDb}, {"dynamicRangeDb", stats.dynamicRangeDb},
                    {"loudestRmsDb", stats.maximumRmsDb}, {"quietestRmsDb", stats.minimumRmsDb},
                    {"perChannel", perChannel}});
        const auto loudness = engine::analyseLoudness(audio, rate);
        out.insert("integratedLufs", double(loudness.integrated));
        out.insert("truePeakDb", double(loudness.truePeak));
        return out;
    }
    if (kind == QLatin1String("spectrum")) {
        const auto shape = window == QLatin1String("blackman") ? render::SpectrumWindow::Blackman
            : window == QLatin1String("rectangular") ? render::SpectrumWindow::Rectangular : render::SpectrumWindow::Hann;
        const auto spectrum = render::frequencySpectrum(audio, 0, audio.frames(), rate, std::uint32_t(fftSize), shape);
        if (!spectrum.ok) return fail(QString::fromStdString(spectrum.error));
        const auto curves = logCurves(spectrum, 1024);
        out.insert({{"fftSize", int(spectrum.fftSize)}, {"curves", curves}});
        return out;
    }
    if (kind == QLatin1String("phase")) {
        const auto phase = render::phaseAnalysis(audio, 0, audio.frames());
        if (!phase.ok) return fail(QString::fromStdString(phase.error));
        out.insert({{"correlation", phase.correlation}, {"balanceDb", phase.balanceDb}, {"sideToMidDb", phase.sideToMidDb}});
        return out;
    }
    return fail(tr("Choose statistics, spectrum or phase."));
}

bool Backend::generateAudio(const QVariantMap& spec)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    if (recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current edit first."));
    auto* lane = m_project.track(m_focused);
    if (!lane) return reject(tr("Choose a track for the generated audio."));
    const auto rate = m_project.sampleRate();
    // Into the selection on this track (replacing it), else at the playhead.
    const bool replace = !m_selection.empty() && m_selection.track == m_focused;
    const session::FramePos start = replace ? m_selection.start : m_playhead;
    const double seconds = spec.value("seconds", 0).toDouble();
    const session::FramePos frames = replace ? m_selection.length() : session::FramePos(std::llround(seconds * rate));
    std::uint32_t channels = std::uint32_t(spec.value("channels", 0).toInt());
    if (!channels) {
        channels = 1;
        for (const auto& clip : lane->clips)
            if (const auto* source = m_project.source(clip.source)) channels = std::max(channels, source->audio.channels());
    }
    std::string error;
    session::AudioBuffer audio;
    const auto kind = spec.value("kind").toString();
    QString label;
    if (kind == QLatin1String("tone")) {
        const auto shapeName = spec.value("shape", "sine").toString();
        const auto shape = shapeName == QLatin1String("square") ? session::ToneShape::Square
            : shapeName == QLatin1String("triangle") ? session::ToneShape::Triangle
            : shapeName == QLatin1String("sawtooth") ? session::ToneShape::Sawtooth : session::ToneShape::Sine;
        audio = session::generateTone({shape, spec.value("frequency", 1000).toDouble(), spec.value("endFrequency", 0).toDouble(),
            spec.value("levelDb", -18).toDouble(), frames, channels, rate}, error);
        label = spec.value("endFrequency", 0).toDouble() > 0 ? tr("Sweep") : tr("Tone %1 Hz").arg(spec.value("frequency", 1000).toDouble());
    } else if (kind == QLatin1String("noise")) {
        const auto colourName = spec.value("colour", "white").toString();
        const auto colour = colourName == QLatin1String("pink") ? session::NoiseColour::Pink
            : colourName == QLatin1String("brown") ? session::NoiseColour::Brown : session::NoiseColour::White;
        audio = session::generateNoise({colour, spec.value("levelDb", -18).toDouble(), frames, channels,
            std::uint64_t(spec.value("seed", 1).toLongLong())}, error);
        label = tr("%1 noise").arg(colourName == QLatin1String("pink") ? tr("Pink") : colourName == QLatin1String("brown") ? tr("Brown") : tr("White"));
    } else return reject(tr("Choose tone or noise."));
    if (audio.empty()) return reject(QString::fromStdString(error));
    if (const auto deferred = deferAutomationEdit([this, spec] { return generateAudio(spec); })) return *deferred;
    setError({});
    const auto before = captureEditState();
    if (audio.frames() * audio.channels() * sizeof(float) >= session::AudioBuffer::kDiskThreshold) audio.storeOnDisk();
    const auto source = m_project.addSource(label.toStdString(), {}, std::move(audio));
    auto* target = m_project.track(m_focused);
    // The generated clip takes the range over; audio around it stays in place.
    session::edits::clearRange(target->clips, start, start + frames);
    session::Clip clip;
    clip.source = source; clip.timelineStart = start; clip.length = frames;
    target->clips.push_back(clip);
    std::stable_sort(target->clips.begin(), target->clips.end(),
                     [](const auto& a, const auto& b) { return a.timelineStart < b.timelineStart; });
    commit(("generate " + label).toStdString(), m_focused, before);
    markPeaksDirty(m_focused);
    pumpPeaks();
    return m_lastError.isEmpty();
}

bool Backend::matchClipLoudness(double targetLufs, const QString& stamp)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    if (stamp.isEmpty() || stamp != automationStamp()) return reject(tr("The project changed. Try again."));
    if (!std::isfinite(targetLufs) || targetLufs < -60 || targetLufs > -5) return reject(tr("Choose a target from -60 to -5 LUFS."));
    if (recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current edit first."));
    std::vector<session::ClipAddress> targets;
    if (selectedClipsCurrent()) targets = m_selectedClips;
    if (targets.empty()) {
        const auto reference = clipProperties();
        if (!reference.value("valid").toBool()) return reject(tr("Select the clips to match first."));
        const auto t = session::TrackId(reference.value("track").toInt());
        const auto index = std::size_t(reference.value("clip").toInt());
        targets.push_back({t, index, m_project.track(t)->clips[index]});
    }
    const auto heard = heardProject();
    if (!heard.missing.empty()) return reject(tr("Clip effects are still being prepared. Try again in a moment."));
    // Measure each clip as it plays at unity gain, then set the gain that reaches the target.
    std::vector<std::pair<session::ClipAddress, float>> gains;
    for (const auto& address : targets) {
        const auto* lane = m_project.track(address.track);
        const auto* heardLane = heard.project.track(address.track);
        if (!lane || !heardLane || address.index >= lane->clips.size()) return reject(tr("A selected clip changed. Select it again."));
        const auto& original = lane->clips[address.index];
        // The heard project keeps clip order and positions; find the same view.
        const session::Clip* view = nullptr;
        for (const auto& candidate : heardLane->clips)
            if (candidate.timelineStart == original.timelineStart && candidate.length == original.length) view = &candidate;
        if (!view) return reject(tr("A selected clip is not audible yet."));
        auto unity = *view; unity.gain = 1;
        const auto* source = heard.project.source(unity.source);
        if (!source) return reject(tr("A selected clip has no audio."));
        if (original.length > kMaxAnalysisSeconds * session::FramePos(m_project.sampleRate()))
            return reject(tr("Clips longer than 20 minutes cannot be matched at once."));
        session::AudioBuffer audio;
        try {
            audio = session::renderClipRange(heard.project, {unity}, unity.timelineStart, unity.timelineEnd(),
                                             std::min<std::uint32_t>(source->audio.channels(), 8));
        } catch (const std::exception& error) { return reject(QString::fromUtf8(error.what())); }
        const auto report = engine::analyseLoudness(audio, m_project.sampleRate());
        if (!(report.integrated > engine::kLoudnessSilence)) return reject(tr("A selected clip is too short or too quiet to measure: loudness needs at least 0.4 seconds above silence."));
        const double gain = std::pow(10.0, (targetLufs - report.integrated) / 20.0);
        if (!std::isfinite(gain) || gain > 64) return reject(tr("A selected clip would need more than +36 dB to reach the target."));
        gains.emplace_back(address, float(gain));
    }
    if (const auto deferred = deferAutomationEdit([this, targetLufs, stamp] { return matchClipLoudness(targetLufs, stamp); })) return *deferred;
    setError({});
    const auto before = captureEditState();
    for (const auto& [address, gain] : gains) {
        auto& clip = m_project.track(address.track)->clips[address.index];
        clip.gain = gain;
        for (auto& selected : m_selectedClips) if (selected.track == address.track && selected.index == address.index) selected.expected = clip;
    }
    commit("match clip loudness", targets.front().track, before);
    return m_lastError.isEmpty();
}

QVariantMap Backend::liveAnalysis(int fftSize)
{
    // The newest programme output (before the monitor level and metronome).
    if (fftSize < 256 || fftSize > int(engine::OutputScope::kFrames) || (fftSize & (fftSize - 1)))
        return {{"ok", false}, {"reason", tr("Choose a live analysis size from 256 to 8192 points.")}};
    std::vector<float> left(static_cast<std::size_t>(fftSize)), right(static_cast<std::size_t>(fftSize));
    const auto written = m_transport.scope().read(left.data(), right.data(), left.size());
    const bool fresh = written != m_liveScopeWritten;
    m_liveScopeWritten = written;
    if (written == 0) return {{"ok", false}, {"reason", tr("Play the project to see live readings.")}};
    session::AudioBuffer audio(2, left.size());
    std::copy(left.begin(), left.end(), audio.plane(0).begin());
    std::copy(right.begin(), right.end(), audio.plane(1).begin());
    const auto spectrum = render::frequencySpectrum(audio, 0, audio.frames(), m_project.sampleRate(), std::uint32_t(fftSize));
    const auto phase = render::phaseAnalysis(audio, 0, audio.frames());
    return {{"ok", true}, {"fresh", fresh}, {"live", true}, {"fftSize", fftSize}, {"curves", logCurves(spectrum, 256)},
            {"correlation", phase.correlation}, {"balanceDb", phase.balanceDb}, {"sideToMidDb", phase.sideToMidDb},
            {"channels", 2}, {"trackName", tr("Live output")}};
}
