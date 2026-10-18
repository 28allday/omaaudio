#pragma once

#include <QString>
#include <QByteArray>

#include "spectral/tiles.h"

#include <atomic>
#include <span>
#include <vector>

// Finished spectral tile sets, kept between sessions under the cache folder.
//
// The key is a digest of the audio that was analysed and of every setting that
// shapes the tiles, so an entry can only ever be the display of that exact audio:
// reopening a project, returning to an earlier edit by Undo, or analysing the
// same take in another window finds its tiles instead of computing them again.
//
// This is a CACHE. An entry that is missing, partial, damaged or from another
// version is not an error, it is a rebuild. Entries are written to a private
// folder and renamed into place, so another OmaAudio never reads half of one;
// several instances share the folder and any of them may prune it.
//
//   <root>/<key>/manifest.json   geometry of the set and of every tile
//   <root>/<key>/c0-l0-t0.png    the tiles, exactly as the builder wrote them
class SpectralStore {
public:
    explicit SpectralStore(QString root = {}) : m_root(std::move(root)) {} // no root: keeps nothing
    [[nodiscard]] const QString &root() const { return m_root; }

    // Worker only. Empty when cancelled; never empty otherwise.
    [[nodiscard]] static QByteArray key(const std::vector<std::span<const float>> &planes,
                                        std::uint32_t sampleRate, const omawave::spectral::StftConfig &config,
                                        bool midSide, const std::atomic<bool> *cancel = nullptr);

    // Copy an entry's tiles to `dir` as `stem`-c…png and describe them in `out`.
    // False, with nothing left in `dir`, when there is no usable entry.
    [[nodiscard]] bool restore(const QByteArray &key, const QString &dir, const QString &stem,
                               omawave::spectral::SpectralSet &out) const;
    // Keep a finished set. False when it could not be stored; the set is unaffected.
    bool save(const QByteArray &key, const omawave::spectral::SpectralSet &set) const;

    struct Usage { qint64 bytes = 0; int entries = 0; };
    [[nodiscard]] Usage usage() const;
    // Remove the entries used longest ago until the folder fits; zero empties it.
    // Abandoned part-written entries go too. Returns how many entries were removed.
    int prune(qint64 limitBytes) const;

private:
    QString m_root;
};
