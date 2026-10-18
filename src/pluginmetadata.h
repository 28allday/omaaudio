#pragma once

#include <QVariantMap>
#include <lilv/lilv.h>

// Presentation only: called while assembling rack descriptors on the GUI thread.
// Never changes a plugin instance, control value, automation or project state.
QVariantMap pluginControlPresentation(LilvWorld *world, const LilvPlugin *plugin,
                                     const LilvPort *port, double minimum, double maximum);

// The presets an LV2 effect ships with (pset:Preset), as {id ("lv2preset:" +
// its URI), name, controls: {symbol: value}}, sorted by name. Loads each preset's
// description into the world once; values outside a port's range are kept as
// written (the caller clamps). Empty for a plugin with none.
QVariantList pluginOwnPresets(LilvWorld *world, const LilvPlugin *plugin);
