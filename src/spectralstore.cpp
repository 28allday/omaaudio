#include "spectralstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

using namespace omawave;

namespace {
// Bump when the tiles or this layout change: older entries simply stop matching.
constexpr int kStoreVersion = 1;
constexpr auto kManifest = "manifest.json";
constexpr auto kPartPrefix = "part-";
constexpr qint64 kAbandonedSeconds = 3600;

QString tileName(std::size_t channel, std::size_t level, std::size_t tile)
{
    return QStringLiteral("c%1-l%2-t%3.png").arg(channel).arg(level).arg(tile);
}

qint64 folderBytes(const QString &path)
{
    qint64 bytes = 0;
    QDirIterator files(path, QDir::Files | QDir::Hidden);
    while (files.hasNext()) { files.next(); bytes += files.fileInfo().size(); }
    return bytes;
}
} // namespace

QByteArray SpectralStore::key(const std::vector<std::span<const float>> &planes, std::uint32_t sampleRate,
                              const spectral::StftConfig &config, bool midSide, const std::atomic<bool> *cancel)
{
    QCryptographicHash hash(QCryptographicHash::Blake2b_256);
    const std::uint64_t header[] = {std::uint64_t(kStoreVersion), sampleRate, config.fftSize, config.hop,
        std::uint64_t(config.window), midSide ? 1u : 0u, spectral::kTileColumns, spectral::kLevelStep,
        std::uint64_t(spectral::kLevel0ByteBudget), std::uint64_t(planes.size()),
        planes.empty() ? 0u : std::uint64_t(planes.front().size())};
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(header), sizeof header));
    constexpr std::size_t block = 1u << 18; // frames; cancellation is checked between blocks
    for (const auto plane : planes) {
        for (std::size_t at = 0; at < plane.size(); at += block) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return {};
            const auto count = std::min(block, plane.size() - at);
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(plane.data() + at), qsizetype(count * sizeof(float))));
        }
    }
    return hash.result().toHex();
}

bool SpectralStore::restore(const QByteArray &key, const QString &dir, const QString &stem,
                            spectral::SpectralSet &out) const
{
    if (key.isEmpty() || m_root.isEmpty()) return false;
    const QDir entry(QDir(m_root).filePath(QString::fromLatin1(key)));
    QFile manifest(entry.filePath(kManifest));
    if (!manifest.open(QIODevice::ReadOnly)) return false;
    const auto root = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    if (root.value("version").toInt() != kStoreVersion) return false;

    spectral::SpectralSet set;
    set.sampleRate = std::uint32_t(root.value("sampleRate").toInteger());
    set.channels = std::uint32_t(root.value("channels").toInteger());
    set.fftSize = std::uint32_t(root.value("fftSize").toInteger());
    set.analysisHop = std::uint32_t(root.value("analysisHop").toInteger());
    set.hop = std::uint32_t(root.value("hop").toInteger());
    set.bins = std::uint32_t(root.value("bins").toInteger());
    set.columns = std::size_t(root.value("columns").toInteger());
    set.tileColumns = std::uint32_t(root.value("tileColumns").toInteger());
    const auto channels = root.value("perChannel").toArray();
    if (!set.channels || !set.bins || !set.columns || !set.tileColumns || channels.size() != qsizetype(set.channels))
        return false;

    QStringList copied;
    const auto fail = [&] { for (const auto &path : copied) QFile::remove(path); return false; };
    for (qsizetype c = 0; c < channels.size(); ++c) {
        spectral::SpectralChannel channel;
        const auto levels = channels.at(c).toArray();
        if (levels.isEmpty()) return fail();
        for (qsizetype l = 0; l < levels.size(); ++l) {
            const auto described = levels.at(l).toObject();
            spectral::SpectralLevel level;
            level.decimation = std::uint32_t(described.value("decimation").toInteger());
            level.columns = std::size_t(described.value("columns").toInteger());
            const auto tiles = described.value("tiles").toArray();
            if (!level.decimation || !level.columns || tiles.isEmpty()) return fail();
            for (qsizetype t = 0; t < tiles.size(); ++t) {
                const auto values = tiles.at(t).toArray();
                if (values.size() != 4) return fail();
                spectral::SpectralTile tile;
                tile.firstColumn = std::size_t(values.at(0).toInteger());
                tile.columns = std::uint32_t(values.at(1).toInteger());
                tile.width = std::uint32_t(values.at(2).toInteger());
                tile.height = std::uint32_t(values.at(3).toInteger());
                const auto name = tileName(std::size_t(c), std::size_t(l), std::size_t(t));
                const auto target = QDir(dir).filePath(stem + QLatin1Char('-') + name);
                if (!tile.columns || !tile.width || !tile.height) return fail();
                // An empty tile is what a crash between write and rename leaves.
                if (QFileInfo(entry.filePath(name)).size() <= 0) return fail();
                QFile::remove(target);
                if (!QFile::copy(entry.filePath(name), target)) return fail();
                copied.append(target);
                tile.path = target.toStdString(); tile.ok = true;
                level.tiles.push_back(std::move(tile));
            }
            channel.levels.push_back(std::move(level));
        }
        set.perChannel.push_back(std::move(channel));
    }
    set.ok = true;
    out = std::move(set);
    // Used now: the entry moves to the back of the queue for pruning. Opened
    // without creating: if another window pruned the entry meanwhile, a new
    // empty manifest must not appear in its place and pass for a stored display.
    if (QFile touched(entry.filePath(kManifest)); touched.open(QIODevice::ReadWrite | QIODevice::ExistingOnly))
        touched.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    return true;
}

bool SpectralStore::save(const QByteArray &key, const spectral::SpectralSet &set) const
{
    if (key.isEmpty() || m_root.isEmpty() || !set.ok || set.perChannel.empty()) return false;
    QDir root(m_root);
    if (!root.mkpath(".")) return false;
    QFile::setPermissions(m_root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const auto name = QString::fromLatin1(key);
    if (QFileInfo::exists(root.filePath(name))) {
        // Already kept, if what is there can be read back. An entry that
        // cannot (a damaged manifest, tiles missing or empty) would otherwise
        // stand in the way for ever: nothing restored from it, nothing stored
        // over it. It is replaced.
        const QString probe = root.filePath(kPartPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces));
        spectral::SpectralSet existing;
        const bool usable = QDir().mkpath(probe) && restore(key, probe, QStringLiteral("probe"), existing);
        QDir(probe).removeRecursively();
        if (usable) return true;
        if (!QDir(root.filePath(name)).removeRecursively()) return false;
    }

    const auto part = root.filePath(kPartPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(part)) return false;
    const auto abandon = [&] { QDir(part).removeRecursively(); return false; };
    QJsonArray channels;
    for (std::size_t c = 0; c < set.perChannel.size(); ++c) {
        QJsonArray levels;
        for (std::size_t l = 0; l < set.perChannel[c].levels.size(); ++l) {
            const auto &level = set.perChannel[c].levels[l];
            QJsonArray tiles;
            for (std::size_t t = 0; t < level.tiles.size(); ++t) {
                const auto &tile = level.tiles[t];
                if (!tile.ok || !QFile::copy(QString::fromStdString(tile.path), QDir(part).filePath(tileName(c, l, t))))
                    return abandon();
                tiles.append(QJsonArray{qint64(tile.firstColumn), qint64(tile.columns), qint64(tile.width), qint64(tile.height)});
            }
            levels.append(QJsonObject{{"decimation", qint64(level.decimation)}, {"columns", qint64(level.columns)}, {"tiles", tiles}});
        }
        channels.append(levels);
    }
    const QJsonObject described{{"version", kStoreVersion}, {"sampleRate", qint64(set.sampleRate)},
        {"channels", qint64(set.channels)}, {"fftSize", qint64(set.fftSize)}, {"analysisHop", qint64(set.analysisHop)},
        {"hop", qint64(set.hop)}, {"bins", qint64(set.bins)}, {"columns", qint64(set.columns)},
        {"tileColumns", qint64(set.tileColumns)}, {"perChannel", channels}};
    QFile manifest(QDir(part).filePath(kManifest));
    const auto body = QJsonDocument(described).toJson(QJsonDocument::Compact);
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(body) != body.size() || !manifest.flush()) return abandon();
    manifest.close();
    // Clearing the cache may have taken this part away while it was being
    // written; an entry is only published whole.
    for (std::size_t c = 0; c < set.perChannel.size(); ++c)
        for (std::size_t l = 0; l < set.perChannel[c].levels.size(); ++l)
            for (std::size_t t = 0; t < set.perChannel[c].levels[l].tiles.size(); ++t)
                if (QFileInfo(QDir(part).filePath(tileName(c, l, t))).size() <= 0) return abandon();
    // Another instance may have stored the same audio meanwhile; either copy will do.
    if (!root.rename(QFileInfo(part).fileName(), name)) abandon();
    return QFileInfo::exists(root.filePath(name + QLatin1Char('/') + kManifest));
}

SpectralStore::Usage SpectralStore::usage() const
{
    Usage out;
    if (m_root.isEmpty()) return out;
    for (const auto &entry : QDir(m_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        out.bytes += folderBytes(entry.absoluteFilePath());
        if (!entry.fileName().startsWith(kPartPrefix)) ++out.entries;
    }
    return out;
}

int SpectralStore::prune(qint64 limitBytes) const
{
    if (m_root.isEmpty()) return 0;
    struct Entry { QString path; QDateTime used; qint64 bytes; };
    std::vector<Entry> entries;
    qint64 total = 0;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &entry : QDir(m_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto path = entry.absoluteFilePath();
        const QFileInfo manifest(QDir(path).filePath(kManifest));
        if (entry.fileName().startsWith(kPartPrefix) || !manifest.exists()) {
            // Still being written by someone unless it has sat untouched for an hour.
            if (limitBytes > 0 && entry.lastModified().secsTo(now) < kAbandonedSeconds) { total += folderBytes(path); continue; }
            QDir(path).removeRecursively();
            continue;
        }
        entries.push_back({path, manifest.lastModified(), folderBytes(path)});
        total += entries.back().bytes;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.used < b.used; });
    int removed = 0;
    for (const auto &entry : entries) {
        if (total <= limitBytes) break;
        if (QDir(entry.path).removeRecursively()) { total -= entry.bytes; ++removed; }
    }
    return removed;
}
