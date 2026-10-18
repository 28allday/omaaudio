#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

// The starting points that ship with OmaAudio (src/presets/*.json, in the
// resources): named settings for effects people commonly use, keyed by the
// effect's URI and by control SYMBOL, never by port number. Read-only; a person
// keeps a changed copy as a preset of their own.
//
// A file is {"plugins": [uri, ...], "presets": [{"id", "name", "group", "note",
// "controls": {symbol: number | {"db": number}}}]}. {"db": x} is a level for a
// control that holds a plain factor (1 = no change): it is stored as 10^(x/20).
// A preset lists only what it changes; every other control is put back to the
// effect's default when it is applied, so a starting point always sounds the same.
struct BuiltInPreset {
    QString id;                     // "builtin:<file id>.<preset id>", unique
    QString name;                   // plain words, shown in the Preset menu
    QString group;                  // Voice, Music, Room… (for the documentation)
    QString note;                   // what it is for, one sentence
    QStringList uris;               // the effects it fits (mono and stereo versions)
    QMap<QString, double> controls; // symbol → stored value
};

// Every shipped preset, read once from :/presets.
const std::vector<BuiltInPreset>& builtInPresets();
// Those that fit one effect, in file order.
std::vector<const BuiltInPreset*> builtInPresetsFor(const QString& uri);
const BuiltInPreset* builtInPreset(const QString& id);
// The first problem found reading the files ("" when they are all well formed).
QString builtInPresetsProblem();
