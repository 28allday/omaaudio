#pragma once
#include "session/project.h"
#include <QString>
#include <QStringList>
#include <atomic>
#include <optional>

namespace omaaudio::aaf {
struct Result {
    std::optional<omawave::session::Project> project;
    QString name, error;
    QString format = QStringLiteral("AAF"); // "AAF" or "OMF", for labels and messages
    QStringList warnings;
    int tracks = 0, clips = 0;
    // The timeline's picture, attached as the session's reference video: its name,
    // empty when none was attached.
    QString video;
};
// Worker-only. The candidate shares the old immutable sources; failure discards
// it. Imported media is private planar storage before anything reaches playback.
Result read(const QString& path, const QString& mediaFolder,
            omawave::session::Project candidate, const std::atomic<bool>* cancel);
// "OMF" for an OMF Interchange (Bento) file, by extension or container label;
// otherwise "AAF".
QString formatOf(const QString& path);
}
