#pragma once

#include <QString>

// The Effects menu's group for an installed plugin, from the class its format
// reports: an LV2 class label ("Reverb Plugin"), the class mapped from CLAP's
// features ("Compressor Plugin") or VST3's sub-categories ("Fx|Reverb"). The three
// formats name things differently; this gives them one set of groups.
//
// Returns a stable key: dynamics, eq, reverb, delay, modulation, distortion,
// space, restoration, pitch, meters, utility, other, or instrument for anything
// that is not an audio effect (instruments, MIDI tools, sources). The interface
// owns the words shown for each.
QString pluginCategory(const QString &className);
