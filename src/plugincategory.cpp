#include "plugincategory.h"

#include <array>
#include <utility>

QString pluginCategory(const QString &className)
{
    const QString c = className.toLower();
    // First match wins, so the more specific groups come first: a VST3
    // "Fx|Spatial|Reverb" is a reverb, an LV2 "Comb Filter" is a filter, and an
    // instrument (or a MIDI tool, or a constant source) is never listed as an
    // audio effect.
    static const std::array<std::pair<const char *, std::array<const char *, 10>>, 12> groups{{
        {"instrument", {"instrument", "synth", "generator", "oscillator", "sampler", "drum", "midi", "constant", nullptr}},
        {"restoration", {"restoration", "denois", "noise reduction", "declick", "de-click", nullptr}},
        {"reverb", {"reverb", nullptr}},
        {"delay", {"delay", "echo", nullptr}},
        {"pitch", {"pitch", "stretch", nullptr}},
        {"modulation", {"modulat", "chorus", "flanger", "phaser", "tremolo", "vibrato", nullptr}},
        {"distortion", {"distortion", "waveshaper", "saturat", "simulator", "overdrive", nullptr}},
        {"dynamics", {"dynamics", "compressor", "limiter", "gate", "expander", "de-esser", "deesser", "envelope", "mastering", nullptr}},
        {"eq", {"eq", "equali", "filter", "lowpass", "highpass", "bandpass", "allpass", nullptr}},
        {"space", {"spatial", "stereo", "panner", "surround", "ambisonic", nullptr}},
        // Meters, scopes and analysers watch the sound and change nothing.
        {"meters", {"analy", "metering", "scope", nullptr}},
        {"utility", {"utility", "tools", "mixer", "converter", "amplifier", "function", nullptr}},
    }};
    for (const auto &[key, words] : groups)
        for (const char *word : words)
            if (word && c.contains(QLatin1String(word)))
                return QLatin1String(key);
    return QStringLiteral("other");
}
