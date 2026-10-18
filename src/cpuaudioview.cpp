#include "cpuaudioview.h"

#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QUrl>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
constexpr qint64 kMaxPixels = 8 * 1024 * 1024;
double number(const QJsonObject &o, const char *key, double fallback = 0)
{
    const double value = o.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? value : fallback;
}
float smooth(float low, float high, float value)
{
    const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
std::array<QColor, 256> palette(const QJsonArray &stops, float floor, float range, float gain)
{
    std::array<QColor, 5> colours;
    for (int i = 0; i < 5; ++i) colours[i] = QColor(stops.at(i).toString());
    constexpr std::array<float, 5> at{0, .45f, .72f, .9f, 1};
    std::array<QColor, 256> result;
    for (int i = 0; i < 256; ++i) {
        const float t = std::clamp((floor * (1 - i / 255.0f) + gain + range) / range, 0.0f, 1.0f);
        int segment = 0;
        while (segment < 3 && t >= at[segment + 1]) ++segment;
        const float f = (t - at[segment]) / (at[segment + 1] - at[segment]);
        const auto &a = colours[segment]; const auto &b = colours[segment + 1];
        result[i] = QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f,
            a.greenF() + (b.greenF() - a.greenF()) * f,
            a.blueF() + (b.blueF() - a.blueF()) * f,
            a.alphaF() + (b.alphaF() - a.alphaF()) * f);
    }
    return result;
}
struct Bins { std::array<int, 8> indices{}; int count = 0; };
}

QImage CpuAudioRenderer::load(const QString &path, QString &error)
{
    // Include replacement identity; generation paths are normally already unique.
    const QFileInfo info(path);
    if (!info.isFile() || !info.isAbsolute() || info.isSymLink()) { error = "Audio display cache is missing or invalid."; return {}; }
    const auto key = path + ':' + QString::number(info.size()) + ':'
        + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&m_mutex);
        if (const auto *found = m_cache.object(key)) return *found;
    }
    QImageReader reader(path);
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || qint64(size.width()) * size.height() > 16 * 1024 * 1024) {
        error = "Audio display cache has invalid dimensions."; return {};
    }
    QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) { error = "Could not read audio display cache: " + reader.errorString(); return {}; }
    const int cost = int((image.sizeInBytes() + 1023) / 1024);
    if (cost <= m_cache.maxCost()) {
        QMutexLocker lock(&m_mutex);
        m_cache.insert(key, new QImage(image), cost);
    }
    return image;
}

int CpuAudioRenderer::cachedKiB()
{
    QMutexLocker lock(&m_mutex);
    return m_cache.totalCost();
}

QImage CpuAudioRenderer::render(const QJsonObject &request, QSize size,
                               const std::atomic_bool &cancel, QString &error)
{
    error.clear();
    if (cancel.load()) return {};
    const double logicalWidth = number(request, "width"), logicalHeight = number(request, "height");
    if (logicalWidth <= 0 || logicalHeight <= 0 || logicalWidth > 1e7 || logicalHeight > 1e7) {
        error = "Invalid audio viewport size."; return {};
    }
    if (size.isEmpty()) size = QSize(int(std::ceil(logicalWidth)), int(std::ceil(logicalHeight)));
    // Preserve the whole viewport at a lower raster resolution on extreme displays.
    const double scale = std::min({1.0, 8192.0 / size.width(), 8192.0 / size.height(),
        std::sqrt(double(kMaxPixels) / (double(size.width()) * size.height()))});
    size = QSize(std::max(1, int(size.width() * scale)), std::max(1, int(size.height() * scale)));
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) { error = "Not enough memory for audio display."; return {}; }
    out.fill(Qt::transparent);
    const double dx = logicalWidth / out.width(), dy = logicalHeight / out.height();
    if (request.value("kind") == "wave") {
        const auto level = request.value("level").toObject();
        const QImage peaks = load(level.value("texture").toString(), error);
        if (peaks.isNull() || cancel.load()) return {};
        const double countValue = number(level, "peakCount");
        const double samplesPerPeak = number(level, "samplesPerPeak");
        const double channelsValue = number(level, "channels"), channelValue = number(request, "channel", -1);
        if (channelsValue < 0 || channelsValue > 8 || channelValue < -1 || channelValue > 7
            || std::floor(channelsValue) != channelsValue || std::floor(channelValue) != channelValue) {
            error = "Invalid waveform channel."; return {};
        }
        const int channels = int(channelsValue), channel = int(channelValue);
        const int block = channel >= 0 && channel < channels ? channel + 1 : 0;
        if (countValue < 1 || std::floor(countValue) != countValue
            || countValue > qint64(peaks.width()) * peaks.height() / (2 * (block + 1))
            || samplesPerPeak <= 0) { error = "Invalid waveform cache geometry."; return {}; }
        const qint64 count = qint64(countValue);
        const double offset = number(request, "viewStart") / samplesPerPeak;
        const double ppp = number(request, "framesPerPixel", 1) / samplesPerPeak;
        if (!std::isfinite(ppp) || !std::isfinite(offset) || ppp <= 0 || std::abs(offset) > 1e15) {
            error = "Invalid waveform view range."; return {};
        }
        const QColor colour(request.value("colour").toString());
        if (!colour.isValid()) { error = "Invalid waveform colour."; return {}; }
        const float feather = .5f / std::max(1.0, logicalWidth) + .005f;
        auto decode = [&](qint64 texel) {
            const QRgb pixel = reinterpret_cast<const QRgb *>(peaks.constScanLine(int(texel / peaks.width())))[texel % peaks.width()];
            return (qRed(pixel) * 256.0f + qGreen(pixel)) * (2.0f / 65535.0f) - 1;
        };
        for (int x = 0; x < out.width(); ++x) {
            if (cancel.load(std::memory_order_relaxed)) return {};
            const double start = offset + std::floor((x + .5) * dx) * ppp, end = start + ppp;
            float low = 1, high = -1; bool any = false;
            for (int tap = 0; tap < 8; ++tap) {
                const double index = std::floor(start) + tap * std::max(1.0, ppp / 8);
                if (index < 0 || index >= count || index >= end) break;
                const qint64 base = (block * count + qint64(index)) * 2;
                high = std::max(high, decode(base)); low = std::min(low, decode(base + 1)); any = true;
            }
            if (!any) continue;
            for (int y = 0; y < out.height(); ++y) {
                const float amplitude = (.5 - (y + .5) / out.height()) / .46;
                const float coverage = smooth(low - feather, low + feather, amplitude)
                    * (1 - smooth(high - feather, high + feather, amplitude));
                const int alpha = qRound(colour.alpha() * coverage);
                reinterpret_cast<QRgb *>(out.scanLine(y))[x] = qPremultiply(qRgba(colour.red(), colour.green(), colour.blue(), alpha));
            }
        }
        return out;
    }
    if (request.value("kind") != "spectral") { error = "Unknown audio display operation."; return {}; }
    const double nyquist = number(request, "nyquist", 24000);
    const double minHz = number(request, "minHz", 20);
    const double range = number(request, "rangeDb", 60), floor = number(request, "floorDb", -120);
    const bool logarithmic = request.value("log").toBool();
    if (nyquist < 1 || nyquist > 1e6 || minHz < .001 || minHz >= nyquist || range < .001 || range > 1000 || floor > -.001 || floor < -1000
        || std::abs(number(request, "gainDb")) > 1000) {
        error = "Invalid spectral view range."; return {};
    }
    const auto layers = request.value("layers").toArray();
    if (layers.isEmpty() || layers.size() > 2) { error = "Invalid spectral channel layers."; return {}; }
    for (qsizetype layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
        const auto layer = layers[layerIndex].toObject();
        const auto stops = layer.value("palette").toArray();
        if (stops.size() != 5) { error = "Invalid spectral palette."; return {}; }
        for (const auto &stop : stops) if (!QColor(stop.toString()).isValid()) { error = "Invalid spectral colour."; return {}; }
        const auto colours = palette(stops, float(floor), float(range), float(number(request, "gainDb")));
        const auto descriptors = layer.value("tiles").toArray();
        if (descriptors.size() > 256) { error = "Too many visible spectral tiles."; return {}; }
        QImage overlay;
        QImage *target = &out;
        if (layerIndex) {
            overlay = QImage(size, QImage::Format_ARGB32_Premultiplied);
            if (overlay.isNull()) { error = "Not enough memory for spectral overlay."; return {}; }
            overlay.fill(Qt::transparent); target = &overlay;
        }
        auto over = [](QRgb source, QRgb dest) {
            const int inverse = 255 - qAlpha(source);
            return qRgba(qRed(source) + (qRed(dest) * inverse + 127) / 255,
                qGreen(source) + (qGreen(dest) * inverse + 127) / 255,
                qBlue(source) + (qBlue(dest) * inverse + 127) / 255,
                qAlpha(source) + (qAlpha(dest) * inverse + 127) / 255);
        };
        // Decode one tile at a time. Session length cannot grow retained tiles
        // beyond the fixed decoded cache and the current tile.
        for (const auto &entry : descriptors) {
            if (cancel.load()) return {};
            const auto t = entry.toObject();
            const double tileX = number(t, "x"), tileWidth = number(t, "width");
            if (tileWidth <= 0 || tileX >= logicalWidth || tileX + tileWidth <= 0) continue;
            const QImage image = load(t.value("texture").toString(), error);
            if (image.isNull()) return {};
            const double columnsValue = number(t, "columns");
            if (columnsValue < 1 || columnsValue > image.width()) { error = "Invalid spectral tile geometry."; return {}; }
            const int columns = int(columnsValue), bins = image.height();
            std::vector<Bins> rows(out.height());
            auto binAt = [&](double u) {
                u = std::clamp(u, 0.0, 1.0);
                return (logarithmic ? minHz * std::pow(nyquist / minHz, u) / nyquist : u) * (bins - 1);
            };
            for (int y = 0; y < out.height(); ++y) {
                const double up = 1 - (y + .5) * dy / logicalHeight;
                const double bin0 = binAt(up), bin1 = binAt(up + 1 / std::max(1.0, logicalHeight));
                for (int tap = 0; tap < 8; ++tap) {
                    const double bin = std::floor(bin0) + tap * std::max(1.0, (bin1 - bin0) / 8);
                    if (tap && bin > bin1) break;
                    rows[y].indices[rows[y].count++] = std::clamp(int(bin), 0, bins - 1);
                }
            }
            const int firstX = int(std::clamp(std::ceil(tileX / dx - .5), 0.0, double(out.width())));
            const int lastX = int(std::clamp(std::ceil((tileX + tileWidth) / dx - .5), 0.0, double(out.width())));
            for (int x = firstX; x < lastX; ++x) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                const double start = ((x + .5) * dx - tileX) / tileWidth * columns;
                const double cpp = columns / std::max(1.0, tileWidth), end = start + cpp;
                std::array<int, 8> samples{}; int sampleCount = 0;
                for (int tap = 0; tap < 8; ++tap) {
                    const double column = std::floor(start) + tap * std::max(1.0, cpp / 8);
                    if (column >= end || column >= columns) break;
                    samples[sampleCount++] = std::clamp(int(column), 0, columns - 1);
                }
                for (int y = 0; y < out.height(); ++y) {
                    int best = 0;
                    for (int b = 0; b < rows[y].count; ++b) {
                        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(rows[y].indices[b]));
                        for (int c = 0; c < sampleCount; ++c) best = std::max(best, qRed(row[samples[c]]));
                    }
                    auto &pixel = reinterpret_cast<QRgb *>(target->scanLine(y))[x];
                    pixel = over(qPremultiply(colours[best].rgba()), pixel);
                }
            }
        }
        if (layerIndex) {
            for (int y = 0; y < out.height(); ++y) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                auto *dest = reinterpret_cast<QRgb *>(out.scanLine(y));
                const auto *source = reinterpret_cast<const QRgb *>(overlay.constScanLine(y));
                for (int x = 0; x < out.width(); ++x) {
                    const QRgb p = source[x];
                    dest[x] = over(qRgba((qRed(p) + 1) / 2, (qGreen(p) + 1) / 2,
                        (qBlue(p) + 1) / 2, (qAlpha(p) + 1) / 2), dest[x]);
                }
            }
        }
    }
    return out;
}

namespace {
class Response final : public QQuickImageResponse, public QRunnable {
public:
    Response(CpuAudioRenderer &renderer, const std::optional<DisplayCache> &cache,
             const QString &id, QSize size, bool rawTexture = false)
        : m_renderer(renderer), m_size(size), m_rawTexture(rawTexture)
    {
        setAutoDelete(false);
        // Qt may defer completion and the producer may retire its model as soon
        // as this call returns. Acquire inputs now, before queuing any work.
        try {
            if (id.size() > 512 * 1024) { m_error = "Audio viewport request is too large."; return; }
            const QByteArray json = id.startsWith('{') ? id.toUtf8() : QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
            const auto document = QJsonDocument::fromJson(json);
            if (!document.isObject()) { m_error = "Invalid audio viewport request."; return; }
            m_request = document.object();
            if (!cache) return; // Standalone CPU reference tests use their own fixtures.
            const auto capture = [&](const QJsonObject &descriptor) {
                const auto token = descriptor.value("asset").toString();
                auto access = cache->acquire(token);
                if (!access.error.isEmpty()) { m_error = access.error; return false; }
                if (access.retired) { m_retired = true; return true; }
                if (!access.lease->contains(descriptor.value("texture").toString())) {
                    m_error = "Audio display path does not belong to its cache batch.";
                    return false;
                }
                // Spectral tiles share one batch; retain each generation once.
                if (std::find(m_leases.begin(), m_leases.end(), access.lease) == m_leases.end())
                    m_leases.push_back(std::move(access.lease));
                return true;
            };
            if (m_rawTexture) capture(m_request);
            else if (m_request.value("kind") == "wave") capture(m_request.value("level").toObject());
            else if (m_request.value("kind") == "spectral") {
                const auto layers = m_request.value("layers").toArray();
                if (layers.isEmpty() || layers.size() > 2) { m_error = "Invalid spectral channel layers."; return; }
                for (const auto &entry : layers) {
                    const auto tiles = entry.toObject().value("tiles").toArray();
                    if (tiles.size() > 256) { m_error = "Too many visible spectral tiles."; return; }
                    for (const auto &tile : tiles) if (!capture(tile.toObject())) return;
                }
            } else m_error = "Unknown audio display operation.";
        } catch (const std::exception &) { m_error = "Could not allocate audio display resources."; }
    }
    void cancel() override { m_cancel.store(true); }
    QString errorString() const override { return m_cancel.load() ? QString{} : m_error; }
    QQuickTextureFactory *textureFactory() const override {
        return m_cancel.load() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    void run() override {
        try {
            if (!m_cancel.load() && m_error.isEmpty()) {
                if (m_retired) {
                    m_image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
                    m_image.fill(Qt::transparent);
                } else if (m_rawTexture) {
                    // The data shader requires original texel dimensions, even
                    // when Qt supplied a sourceSize for the obsolete model.
                    m_image = m_renderer.load(m_request.value("texture").toString(), m_error);
                } else m_image = m_renderer.render(m_request, m_size, m_cancel, m_error);
            }
        } catch (const std::exception &) { m_error = "Could not allocate audio display resources."; }
        m_leases.clear(); // Decoded pixels no longer need their backing PNGs.
        // Cancellation can arrive after decoding or failure, while Qt still has
        // the finished notification queued. The getters also cover that window.
        if (m_cancel.load()) { m_image = {}; m_error.clear(); }
        // Qt owns and deletes the response after this signal, including cancellation.
        emit finished();
    }
private:
    CpuAudioRenderer &m_renderer;
    QJsonObject m_request;
    QString m_error;
    QSize m_size;
    QImage m_image;
    std::vector<DisplayCache::Lease> m_leases;
    bool m_rawTexture = false, m_retired = false;
    std::atomic_bool m_cancel{false};
};
}
CpuAudioViewProvider::CpuAudioViewProvider(std::optional<DisplayCache> cache,
                                         std::shared_ptr<CpuAudioRenderer> renderer)
    : m_cache(std::move(cache)), m_renderer(std::move(renderer)) { m_pool.setMaxThreadCount(2); }
CpuAudioViewProvider::~CpuAudioViewProvider() { m_pool.waitForDone(); }
QQuickImageResponse *CpuAudioViewProvider::requestImageResponse(const QString &id, const QSize &size)
{
    auto *response = new Response(*m_renderer, m_cache, id, size);
    m_pool.start(response);
    return response;
}
AudioTextureProvider::AudioTextureProvider(DisplayCache cache, std::shared_ptr<CpuAudioRenderer> renderer)
    : m_cache(std::move(cache)), m_renderer(std::move(renderer)) { m_pool.setMaxThreadCount(2); }
AudioTextureProvider::~AudioTextureProvider() { m_pool.waitForDone(); }
QQuickImageResponse *AudioTextureProvider::requestImageResponse(const QString &id, const QSize &size)
{
    auto *response = new Response(*m_renderer, m_cache, id, size, true);
    m_pool.start(response);
    return response;
}
