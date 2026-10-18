#include "effectmenu.h"

#include <QFile>
#include <QHash>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <string_view>

#include <toml++/toml.hpp>

namespace {

// The groups in menu order. The interface owns the words shown for each.
const QStringList &groupOrder()
{
    static const QStringList order{
        QStringLiteral("dynamics"), QStringLiteral("eq"), QStringLiteral("reverb"),
        QStringLiteral("delay"), QStringLiteral("modulation"), QStringLiteral("distortion"),
        QStringLiteral("space"), QStringLiteral("restoration"), QStringLiteral("pitch"),
        QStringLiteral("meters"), QStringLiteral("utility"), QStringLiteral("other")};
    return order;
}

// Which copy of one family/variant to keep when a plugin is installed in more
// than one format: LV2, then CLAP, then VST3.
int formatRank(const QString &format)
{
    if (format == QLatin1String("LV2")) return 0;
    if (format == QLatin1String("CLAP")) return 1;
    return 2;
}

// The stereo version first: it is what a plain click adds.
int variantRank(const QString &variant)
{
    static const QStringList order{QStringLiteral("stereo"), QStringLiteral("either"), QStringLiteral("mono"),
                                   QStringLiteral("leftright"), QStringLiteral("midside")};
    const auto i = order.indexOf(variant);
    return i < 0 ? order.size() : i;
}

struct Family {
    QString key, name, category;
    bool sidechain = false;
    QMap<int, QVariantMap> variants;   // variantRank -> chosen plugin entry
};

QVariantMap entryFor(const Family &family)
{
    const QVariantMap &first = family.variants.first();
    QVariantMap entry{
        {QStringLiteral("name"), family.name},
        {QStringLiteral("family"), family.key},
        {QStringLiteral("uri"), first.value(QStringLiteral("uri"))},
        {QStringLiteral("author"), first.value(QStringLiteral("author"))},
        {QStringLiteral("format"), first.value(QStringLiteral("format"))},
        {QStringLiteral("category"), family.category},
    };
    // Only Left/Right and Mid/Side are worth a choice: on a stereo chain the mono
    // version is the stereo one run twice, unlinked.
    QVariantList variants;
    const bool choice = family.variants.contains(variantRank(QStringLiteral("leftright")))
                     || family.variants.contains(variantRank(QStringLiteral("midside")));
    if (choice) {
        const int mono = variantRank(QStringLiteral("mono"));
        for (auto it = family.variants.cbegin(); it != family.variants.cend(); ++it) {
            if (it.key() == mono && it != family.variants.cbegin()) continue;
            variants.append(QVariantMap{{QStringLiteral("variant"), it->value(QStringLiteral("familyVariant"))},
                                        {QStringLiteral("uri"), it->value(QStringLiteral("uri"))}});
        }
    }
    entry.insert(QStringLiteral("variants"), variants);
    return entry;
}

}  // namespace

PluginFamily pluginFamily(const QString &name, const QString &layout)
{
    PluginFamily out;
    QString key = name.trimmed();
    // "BBC Meter (Mono)", "Compressor Stereo", "Multiband Gate MidSide x8": the
    // channel word comes last, or just before a band count.
    static const QRegularExpression parenthesised(QStringLiteral(R"(\s*\((Mono|Stereo)\)$)"));
    static const QRegularExpression word(QStringLiteral(R"(\s+(Mono|Stereo|LeftRight|MidSide)(\s+x\d+)?$)"));
    static const QRegularExpression count(QStringLiteral(R"(\s+x([12])$)"));
    QString variant;
    if (const auto m = parenthesised.match(key); m.hasMatch()) {
        variant = m.captured(1).toLower();
        key.chop(m.capturedLength());
    } else if (const auto m = word.match(key); m.hasMatch()) {
        variant = m.captured(1).toLower();
        key = key.left(m.capturedStart()) + m.captured(2);
    } else if (const auto m = count.match(key); m.hasMatch()
               && layout == (m.captured(1) == QLatin1String("1") ? QLatin1String("mono") : QLatin1String("stereo"))) {
        // "Spectrum Analyzer x1" / "x2": a channel count, not a band count.
        variant = layout;
        key.chop(m.capturedLength());
    }
    out.variant = variant.isEmpty() ? (layout == QLatin1String("none") ? QStringLiteral("stereo") : layout) : variant;
    out.key = key;
    out.name = key;
    // "LSP Sidechain Compressor": a copy keyed from another track. Not
    // "Ring Modulated Sidechain", where the word is the effect itself.
    static const QRegularExpression sidechain(QStringLiteral(R"(\bSidechain\s+(?=\S))"));
    if (const auto m = sidechain.match(key); m.hasMatch()) {
        out.sidechain = true;
        out.name = key.left(m.capturedStart()) + key.mid(m.capturedEnd());
    }
    return out;
}

CoreEffects loadCoreEffects(const QString &path, QString *error)
{
    CoreEffects core;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return core;
    }
    const QByteArray text = file.readAll();
    toml::table table;
    try {
        table = toml::parse(std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
    } catch (const toml::parse_error &e) {
        if (error) *error = QString::fromUtf8(e.description().data(), static_cast<qsizetype>(e.description().size()));
        return core;
    }
    for (const QString &group : groupOrder()) {
        const auto *array = table[group.toStdString()].as_array();
        if (!array) continue;
        QStringList families;
        for (const auto &node : *array)
            if (const auto value = node.value<std::string>()) families.append(QString::fromStdString(*value));
        if (!families.isEmpty()) core.append({group, families});
    }
    return core;
}

QVariantMap buildEffectMenu(const QVariantList &plugins, const QStringList &disabled, const CoreEffects &core)
{
    // Family key -> its usable variants. Instruments (and MIDI tools, sources)
    // are not effects; disabled plugins stay in the browser only.
    QHash<QString, Family> families;
    for (const QVariant &value : plugins) {
        QVariantMap plugin = value.toMap();
        const QString uri = plugin.value(QStringLiteral("uri")).toString();
        const QString category = plugin.value(QStringLiteral("category")).toString();
        if (!plugin.value(QStringLiteral("usable")).toBool() || category == QLatin1String("instrument")
            || disabled.contains(uri))
            continue;
        const PluginFamily f = pluginFamily(plugin.value(QStringLiteral("name")).toString(),
                                            plugin.value(QStringLiteral("layout")).toString());
        // The same family can be read differently by two packs (another author's
        // "Compressor Stereo"): the author keeps them apart.
        const QString id = plugin.value(QStringLiteral("author")).toString() + QChar(u'\n') + f.key;
        Family &family = families[id];
        if (family.key.isEmpty()) {
            family.key = f.key; family.name = f.name; family.sidechain = f.sidechain; family.category = category;
        }
        plugin.insert(QStringLiteral("familyVariant"), f.variant);
        const int rank = variantRank(f.variant);
        const auto existing = family.variants.constFind(rank);
        if (existing == family.variants.cend()
            || formatRank(plugin.value(QStringLiteral("format")).toString())
               < formatRank(existing->value(QStringLiteral("format")).toString()))
            family.variants.insert(rank, plugin);
    }

    const auto byName = [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value(QStringLiteral("name")).toString(),
                                           b.toMap().value(QStringLiteral("name")).toString()) < 0;
    };
    QHash<QString, QVariantList> entries, sidechains;
    QHash<QString, QVariantMap> byKey;   // family key -> entry, for the core set
    for (const Family &family : std::as_const(families)) {
        const QVariantMap entry = entryFor(family);
        (family.sidechain ? sidechains : entries)[family.category].append(entry);
        if (!byKey.contains(family.key)) byKey.insert(family.key, entry);
    }

    QVariantList all;
    for (const QString &key : groupOrder()) {
        QVariantList list = entries.value(key), side = sidechains.value(key);
        if (list.isEmpty() && side.isEmpty()) continue;
        std::sort(list.begin(), list.end(), byName);
        std::sort(side.begin(), side.end(), byName);
        all.append(QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("entries"), list},
                               {QStringLiteral("sidechain"), side}});
    }

    QVariantList coreGroups;
    for (const auto &[key, names] : core) {
        QVariantList list;
        for (const QString &name : names)
            if (const auto it = byKey.constFind(name); it != byKey.cend()) list.append(*it);
        if (!list.isEmpty())
            coreGroups.append(QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("entries"), list},
                                          {QStringLiteral("sidechain"), QVariantList{}}});
    }
    return {{QStringLiteral("core"), coreGroups}, {QStringLiteral("all"), all}};
}
