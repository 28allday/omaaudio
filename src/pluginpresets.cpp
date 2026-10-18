#include "pluginpresets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr int kPresetLimit = 256, kControlLimit = 4096;
constexpr qint64 kFileLimit = 8 * 1024 * 1024;
bool validName(const QString &name)
{
    return !name.isEmpty() && name.size() <= 96 && name == name.trimmed()
        && std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.category() == QChar::Other_Control; });
}
bool validUri(const QString &uri)
{
    const QUrl parsed(uri, QUrl::StrictMode);
    return !uri.isEmpty() && uri.size() <= 4096 && parsed.isValid() && !parsed.scheme().isEmpty()
        && std::none_of(uri.cbegin(), uri.cend(), [](QChar c) { return c.category() == QChar::Other_Control; });
}
bool numericParameters(const QString& uri) { return uri.startsWith("clap:") || uri.startsWith("vst3:"); }
bool exactFloat(double value) {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
        && double(static_cast<float>(value)) == value;
}
bool validControls(const QMap<QString, double> &controls, const QString& uri, bool parameterIds)
{
    static const QRegularExpression symbol(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,127}$"));
    static const QRegularExpression decimal(QStringLiteral("^(0|[1-9][0-9]{0,9})$"));
    if (controls.isEmpty() || controls.size() > kControlLimit) return false;
    if (parameterIds && !numericParameters(uri)) return false;
    for (auto it = controls.cbegin(); it != controls.cend(); ++it) {
        if (!std::isfinite(it.value())) return false;
        if (!parameterIds) {
            if (!symbol.match(it.key()).hasMatch()) return false;
        } else {
            const auto limit = uri.startsWith("vst3:") ? 0x7FFFFFFFULL : 0xFFFFFFFEULL;
            if (!decimal.match(it.key()).hasMatch() || it.key().toULongLong() > limit) return false;
            if (uri.startsWith("vst3:") && (it.value() < 0 || it.value() > 1)) return false;
        }
    }
    return true;
}
QString lockError() { return QStringLiteral("The preset library is busy or unavailable. Try again after the other instance finishes."); }
}

PluginPresets::PluginPresets(QString path) : m_path(std::move(path)) {}

QString PluginPresets::load()
{
    QFile file(m_path);
    if (!file.exists()) { m_entries.clear(); return {}; }
    if (!file.open(QIODevice::ReadOnly) || file.size() > kFileLimit)
        return QStringLiteral("Could not read the parameter preset library: %1").arg(m_path);
    const auto bytes = file.read(kFileLimit + 1);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    const auto object = doc.object();
    const bool withStates = object.value("version") == QJsonValue(3);
    const bool precise = withStates || object.value("version") == QJsonValue(2);
    const auto invalid = [&] { return QStringLiteral("The parameter preset library has an invalid or unsupported format: %1").arg(m_path); };
    if (bytes.size() > kFileLimit || error.error != QJsonParseError::NoError || !doc.isObject()
        || (!precise && object.value("version") != QJsonValue(1)) || !object.value("presets").isArray()
        || object.value("presets").toArray().size() > kPresetLimit) return invalid();
    QList<PluginParameterPreset> entries;
    QSet<QString> ids;
    for (const auto &value : object.value("presets").toArray()) {
        if (!value.isObject()) return invalid();
        const auto row = value.toObject();
        if (!row.value("id").isString() || !row.value("name").isString() || !row.value("uri").isString()
            || !row.value("controls").isObject()) return invalid();
        PluginParameterPreset entry{row.value("id").toString(), row.value("name").toString(), row.value("uri").toString(), {}};
        if (precise && !row.value("parameterIds").isBool()) return invalid();
        entry.parameterIds = precise && row.value("parameterIds").toBool();
        if (row.contains("states")) {
            const auto states = row.value("states").toArray();
            if (!withStates || !row.value("states").isArray() || states.isEmpty() || states.size() > 2) return invalid();
            for (const auto& state : states) {
                const QUuid stateId(state.toString());
                if (!state.isString() || stateId.isNull() || stateId.toString(QUuid::WithoutBraces) != state.toString()) return invalid();
                entry.states.append(state.toString());
            }
        }
        const QUuid uuid(entry.id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != entry.id || ids.contains(entry.id)
            || !validName(entry.name) || !validUri(entry.uri)) return invalid();
        const auto controls = row.value("controls").toObject();
        if (controls.size() > kControlLimit) return invalid();
        for (auto it = controls.constBegin(); it != controls.constEnd(); ++it) {
            if (!it.value().isDouble()) return invalid();
            const double number = it.value().toDouble();
            if (!std::isfinite(number) || (!precise && !exactFloat(number))) return invalid();
            entry.controls.insert(it.key(), number);
        }
        if (!validControls(entry.controls,entry.uri,entry.parameterIds)) return invalid();
        for (const auto &previous : entries)
            if (previous.uri == entry.uri && previous.name == entry.name) return invalid();
        ids.insert(entry.id); entries.append(std::move(entry));
    }
    m_entries = std::move(entries);
    return {};
}

QString PluginPresets::stateRoot() const { return QFileInfo(m_path).absolutePath(); }

QString PluginPresets::write(const QList<PluginParameterPreset> &entries)
{
    // Version 3 only when a complete-settings preset exists, so older builds
    // keep reading libraries of parameter presets.
    const bool withStates = std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) { return !entry.states.isEmpty(); });
    const bool precise = std::any_of(entries.cbegin(),entries.cend(),[](const auto& entry) {
        return entry.parameterIds || std::any_of(entry.controls.cbegin(),entry.controls.cend(),[](double value) { return !exactFloat(value); });
    });
    QJsonArray rows;
    for (const auto &entry : entries) {
        QJsonObject controls;
        for (auto it = entry.controls.cbegin(); it != entry.controls.cend(); ++it) controls.insert(it.key(), double(it.value()));
        QJsonObject row{{"id", entry.id}, {"name", entry.name}, {"uri", entry.uri}, {"controls", controls}};
        if (precise || withStates) row.insert("parameterIds",entry.parameterIds);
        if (!entry.states.isEmpty()) row.insert("states", QJsonArray::fromStringList(entry.states));
        rows.append(row);
    }
    const auto bytes = QJsonDocument(QJsonObject{{"version", withStates ? 3 : precise ? 2 : 1}, {"presets", rows}}).toJson();
    if (entries.size() > kPresetLimit || bytes.size() > kFileLimit)
        return QStringLiteral("The parameter preset library has reached its size limit (256 presets / 8 MiB).");
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("Could not save the parameter preset library: %1").arg(m_path);
    m_entries = entries;
    return {};
}

QString PluginPresets::add(QString name, const QString &uri, const QMap<QString, double> &controls, QString &id,
                           const QStringList &states)
{
    id.clear(); name = name.trimmed();
    if (!validName(name)) return QStringLiteral("Use a preset name of 1–96 characters without line breaks or control characters.");
    const bool parameterIds = numericParameters(uri);
    if (!validUri(uri) || !validControls(controls,uri,parameterIds)) return QStringLiteral("This plugin has invalid or unsupported parameter values.");
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) return lockError();
    QLockFile lock(m_path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return lockError();
    if (const auto error = load(); !error.isEmpty()) return error;
    for (const auto &entry : m_entries)
        if (entry.uri == uri && entry.name == name) return QStringLiteral("A preset with this name already exists for this plugin. Choose another name.");
    auto entries = m_entries;
    const auto created = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entries.append({created, name, uri, controls, parameterIds, states});
    if (const auto error = write(entries); !error.isEmpty()) return error;
    id = created;
    return {};
}

QString PluginPresets::remove(const QString &id)
{
    QLockFile lock(m_path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return lockError();
    if (const auto error = load(); !error.isEmpty()) return error;
    auto entries = m_entries;
    QStringList released;
    for (const auto& entry : entries) if (entry.id == id) released = entry.states;
    const auto removed = entries.removeIf([&](const auto &entry) { return entry.id == id; });
    if (!removed) return QStringLiteral("This preset no longer exists. Reload the library.");
    if (const auto error = write(entries); !error.isEmpty()) return error;
    // State files are immutable and only referenced here; drop the ones no preset uses.
    for (const auto& state : released) {
        if (std::any_of(entries.cbegin(), entries.cend(), [&](const auto& entry) { return entry.states.contains(state); })) continue;
        QFile::remove(stateRoot() + QStringLiteral("/state/") + state + QStringLiteral(".omas"));
    }
    return {};
}
