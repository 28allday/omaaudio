#pragma once
#include <QStringList>

namespace omaaudio::agent {
// A separate entry point: no window, QML engine or audio device unless requested.
bool requested(const QStringList &arguments);
int run(int &argc, char **argv, const QStringList &arguments);
}
