#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// The Effects menu, built from the plugin catalogue.
//
// Plugin packs ship one effect many times: LSP as Mono, Stereo, LeftRight and
// MidSide (and a Sidechain copy of each), x42 as "(Mono)" and "(Stereo)". The
// menu lists the effect once -- a family -- and adds the stereo version, since
// every track chain is stereo (a mono plugin runs there as a pair). Left/Right
// and Mid/Side are separate choices beside it; Sidechain copies sit in their own
// submenu.

// One plugin's place in its family, from its name and channel layout.
struct PluginFamily {
    QString key;        // the name without the channel variant: "LSP Sidechain Compressor"
    QString name;       // as listed: "LSP Compressor" (Sidechain is said by its submenu)
    QString variant;    // stereo, mono, either, leftright or midside
    bool sidechain = false;
};
PluginFamily pluginFamily(const QString &name, const QString &layout);

// The core set: group key -> family keys, in the order the file lists them.
using CoreEffects = QList<QPair<QString, QStringList>>;
// Reads a core-set file (TOML: group = ["family", ...]). Empty when the file is
// missing or does not parse, with `error` saying why.
CoreEffects loadCoreEffects(const QString &path, QString *error = nullptr);

// The menu from Backend.plugins entries (usable, enabled effects only):
//   { core: [group], all: [group] }
//   group = { key, entries: [entry], sidechain: [entry] }   (sidechain: all only)
//   entry = { name, family, uri, author, format, category,
//             variants: [{ variant, uri }] }   (empty unless Left/Right or
//             Mid/Side exist; then the first is `uri`'s own)
// Groups follow one fixed order; core entries follow the file, the rest are by name.
QVariantMap buildEffectMenu(const QVariantList &plugins, const QStringList &disabled, const CoreEffects &core);
