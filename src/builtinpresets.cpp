#include "builtinpresets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cmath>

namespace {

struct Library {
    std::vector<BuiltInPreset> presets;
    QString problem;
};

const Library& library()
{
    static const Library read = [] {
        Library out;
        const auto fail = [&](const QString& problem) { if (out.problem.isEmpty()) out.problem = problem; };
        QSet<QString> ids;
        const QDir dir(QStringLiteral(":/presets"));
        for (const auto& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile in(dir.filePath(file));
            if (!in.open(QIODevice::ReadOnly)) { fail(file + QStringLiteral(": cannot be read")); continue; }
            QJsonParseError parse{};
            const auto doc = QJsonDocument::fromJson(in.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
                fail(file + QStringLiteral(": ") + parse.errorString()); continue;
            }
            const auto root = doc.object();
            QStringList uris;
            for (const auto& uri : root.value(QStringLiteral("plugins")).toArray()) uris << uri.toString();
            if (uris.isEmpty()) { fail(file + QStringLiteral(": no plugins")); continue; }
            const auto stem = QFileInfo(file).completeBaseName();
            for (const auto& value : root.value(QStringLiteral("presets")).toArray()) {
                const auto entry = value.toObject();
                BuiltInPreset preset;
                preset.id = QStringLiteral("builtin:") + stem + QLatin1Char('.') + entry.value(QStringLiteral("id")).toString();
                preset.name = entry.value(QStringLiteral("name")).toString().trimmed();
                preset.group = entry.value(QStringLiteral("group")).toString();
                preset.note = entry.value(QStringLiteral("note")).toString();
                preset.uris = uris;
                if (preset.name.isEmpty() || entry.value(QStringLiteral("id")).toString().isEmpty()) {
                    fail(file + QStringLiteral(": a preset without an id or a name")); continue;
                }
                if (ids.contains(preset.id)) { fail(preset.id + QStringLiteral(": used twice")); continue; }
                const auto controls = entry.value(QStringLiteral("controls")).toObject();
                bool ok = !controls.isEmpty();
                for (auto it = controls.begin(); it != controls.end(); ++it) {
                    double stored = 0;
                    if (it.value().isDouble()) stored = it.value().toDouble();
                    else if (it.value().isObject() && it.value().toObject().value(QStringLiteral("db")).isDouble())
                        stored = std::pow(10.0, it.value().toObject().value(QStringLiteral("db")).toDouble() / 20.0);
                    else { fail(preset.id + QStringLiteral(": ") + it.key() + QStringLiteral(" is not a number")); ok = false; break; }
                    if (!std::isfinite(stored)) { fail(preset.id + QStringLiteral(": ") + it.key() + QStringLiteral(" is not finite")); ok = false; break; }
                    preset.controls.insert(it.key(), stored);
                }
                if (!ok) { if (controls.isEmpty()) fail(preset.id + QStringLiteral(": no controls")); continue; }
                ids.insert(preset.id);
                out.presets.push_back(std::move(preset));
            }
        }
        return out;
    }();
    return read;
}

}  // namespace

const std::vector<BuiltInPreset>& builtInPresets() { return library().presets; }

std::vector<const BuiltInPreset*> builtInPresetsFor(const QString& uri)
{
    std::vector<const BuiltInPreset*> out;
    for (const auto& preset : library().presets)
        if (preset.uris.contains(uri)) out.push_back(&preset);
    return out;
}

const BuiltInPreset* builtInPreset(const QString& id)
{
    for (const auto& preset : library().presets)
        if (preset.id == id) return &preset;
    return nullptr;
}

QString builtInPresetsProblem() { return library().problem; }
