#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

struct PluginParameterPreset {
    QString id, name, uri;
    QMap<QString, double> controls;
    bool parameterIds = false; // version-1 CLAP presets used display names
    // The effect's own complete settings (one per actual instance), stored as
    // state files beside the library. Empty for parameter-only presets.
    QStringList states;
    bool operator==(const PluginParameterPreset &) const = default;
};

// GUI-thread metadata store. Atomic, bounded writes; separate application
// instances merge mutations under a nonblocking lock. No project/DSP state.
class PluginPresets {
public:
    explicit PluginPresets(QString path);
    [[nodiscard]] const QList<PluginParameterPreset> &entries() const { return m_entries; }
    [[nodiscard]] QString load();
    [[nodiscard]] QString add(QString name, const QString &uri, const QMap<QString, double> &controls, QString &id,
                              const QStringList &states = {});
    // Where complete-settings state files live (<library folder>/state).
    [[nodiscard]] QString stateRoot() const;
    [[nodiscard]] QString remove(const QString &id);
private:
    [[nodiscard]] QString write(const QList<PluginParameterPreset> &entries);
    QString m_path;
    QList<PluginParameterPreset> m_entries;
};
