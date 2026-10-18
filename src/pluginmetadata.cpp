#include "pluginmetadata.h"

#include <QMap>
#include <cmath>
#include <memory>
#include <lv2/core/lv2.h>
#include <lv2/port-props/port-props.h>
#include <lv2/units/units.h>

namespace {
using Node = std::unique_ptr<LilvNode, decltype(&lilv_node_free)>;
Node uri(LilvWorld *world, const char *value) { return Node(lilv_new_uri(world, value), lilv_node_free); }
QString label(const LilvNode *node, int limit)
{
    if (!node || !lilv_node_is_string(node)) return {};
    const auto *text = lilv_node_as_string(node);
    return text ? QString::fromUtf8(text).trimmed().left(limit) : QString{};
}
}

QVariantMap pluginControlPresentation(LilvWorld *world, const LilvPlugin *plugin,
                                     const LilvPort *port, double minimum, double maximum)
{
    QVariantMap result{{"unit", QString{}}, {"logarithmic", false}, {"choices", QVariantList{}}, {"group", QString{}}};
    if (!world || !plugin || !port || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum)
        return result;

    // The plugin's own port group (pg:group), by its label, then name, then symbol.
    const auto groupKey = uri(world, "http://lv2plug.in/ns/ext/port-groups#group");
    if (const Node group(lilv_port_get(plugin, port, groupKey.get()), lilv_node_free);
        group && (lilv_node_is_uri(group.get()) || lilv_node_is_blank(group.get()))) {
        for (const char *key : {"http://www.w3.org/2000/01/rdf-schema#label", LV2_CORE__name, LV2_CORE__symbol}) {
            const auto property = uri(world, key);
            const Node text(lilv_world_get(world, group.get(), property.get(), nullptr), lilv_node_free);
            if (const auto name = label(text.get(), 48); !name.isEmpty()) { result["group"] = name; break; }
        }
    }
    const auto logarithmic = uri(world, LV2_PORT_PROPS__logarithmic);
    result["logarithmic"] = minimum > 0 && maximum > minimum
        && lilv_port_has_property(plugin, port, logarithmic.get());

    const auto unit = uri(world, LV2_UNITS__unit), symbol = uri(world, LV2_UNITS__symbol);
    std::unique_ptr<LilvNodes, decltype(&lilv_nodes_free)> units(
        lilv_port_get_value(plugin, port, unit.get()), lilv_nodes_free);
    if (const auto *type = lilv_nodes_get_first(units.get()); type && (lilv_node_is_uri(type) || lilv_node_is_blank(type))) {
        const Node text(lilv_world_get(world, type, symbol.get(), nullptr), lilv_node_free);
        result["unit"] = label(text.get(), 32);
    }

    const auto enumeration = uri(world, LV2_CORE__enumeration);
    if (!lilv_port_has_property(plugin, port, enumeration.get())) return result;
    std::unique_ptr<LilvScalePoints, decltype(&lilv_scale_points_free)> points(
        lilv_port_get_scale_points(plugin, port), lilv_scale_points_free);
    if (lilv_scale_points_size(points.get()) > 256) return result;
    const auto integer = uri(world, LV2_CORE__integer);
    const bool whole = lilv_port_has_property(plugin, port, integer.get());
    QMap<double, QString> choices;
    LILV_FOREACH(scale_points, i, points.get()) {
        const auto *point = lilv_scale_points_get(points.get(), i);
        const auto *valueNode = lilv_scale_point_get_value(point);
        if (!valueNode || (!lilv_node_is_float(valueNode) && !lilv_node_is_int(valueNode))) return result;
        const double value = lilv_node_as_float(valueNode);
        if (!std::isfinite(value) || value < minimum || value > maximum || (whole && value != std::round(value)))
            return result;
        auto text = label(lilv_scale_point_get_label(point), 256);
        if (text.isEmpty()) text = QString::number(value, 'g', 7);
        if (choices.contains(value) && choices.value(value) != text) return result;
        choices[value] = text;
    }
    QVariantList list;
    for (auto it = choices.cbegin(); it != choices.cend(); ++it)
        list.append(QVariantMap{{"value", it.key()}, {"label", it.value()}});
    result["choices"] = list;
    return result;
}

QVariantList pluginOwnPresets(LilvWorld *world, const LilvPlugin *plugin)
{
    QVariantList out;
    if (!world || !plugin) return out;
    const auto preset = uri(world, "http://lv2plug.in/ns/ext/presets#Preset");
    const auto valueKey = uri(world, "http://lv2plug.in/ns/ext/presets#value");
    const auto rdfsLabel = uri(world, "http://www.w3.org/2000/01/rdf-schema#label");
    const auto portKey = uri(world, LV2_CORE__port);
    const auto symbolKey = uri(world, LV2_CORE__symbol);
    std::unique_ptr<LilvNodes, decltype(&lilv_nodes_free)> related(lilv_plugin_get_related(plugin, preset.get()), lilv_nodes_free);
    QMap<QString, QVariantMap> byName;
    LILV_FOREACH(nodes, i, related.get()) {
        const auto *node = lilv_nodes_get(related.get(), i);
        if (!node || !lilv_node_is_uri(node) || byName.size() >= 256) continue;
        lilv_world_load_resource(world, node);
        const Node name(lilv_world_get(world, node, rdfsLabel.get(), nullptr), lilv_node_free);
        const auto text = label(name.get(), 120);
        if (text.isEmpty()) continue;
        std::unique_ptr<LilvNodes, decltype(&lilv_nodes_free)> ports(
            lilv_world_find_nodes(world, node, portKey.get(), nullptr), lilv_nodes_free);
        QVariantMap controls;
        LILV_FOREACH(nodes, j, ports.get()) {
            const auto *port = lilv_nodes_get(ports.get(), j);
            const Node symbol(lilv_world_get(world, port, symbolKey.get(), nullptr), lilv_node_free);
            const Node value(lilv_world_get(world, port, valueKey.get(), nullptr), lilv_node_free);
            if (!symbol || !value || !(lilv_node_is_float(value.get()) || lilv_node_is_int(value.get()))) continue;
            const double v = lilv_node_as_float(value.get());
            if (std::isfinite(v)) controls.insert(QString::fromUtf8(lilv_node_as_string(symbol.get())), v);
        }
        if (controls.isEmpty()) continue;
        byName.insert(text.toLower() + QChar(0) + QString::fromUtf8(lilv_node_as_uri(node)),
            QVariantMap{{"id", QStringLiteral("lv2preset:") + QString::fromUtf8(lilv_node_as_uri(node))},
                        {"name", text}, {"controls", controls}});
    }
    for (const auto &entry : byName) out.append(entry);
    return out;
}
