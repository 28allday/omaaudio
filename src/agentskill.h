#pragma once

#include <QStringList>

namespace omaaudio::skill {
// A QCoreApplication-only entry point: no audio, settings or graphics startup.
bool requested(const QStringList &arguments);
int run(const QStringList &arguments);
}
