#include "storagepaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>

namespace omaaudio::storage {

namespace {

QString validRoot(const QString &candidate, const QString &fallback)
{
    if (candidate.isEmpty() || !candidate.startsWith(QLatin1Char('/')))
        return fallback;
    return QDir::cleanPath(candidate);
}

Migration g_launchMigration;

} // namespace

Locations locationsFor(const QString &configHome, const QString &stateHome,
                       const QString &cacheHome, const QString &runtimeDir,
                       const QString &tmpDir, const QString &home)
{
    const QString app = QString::fromLatin1(appDirName);
    const QString cleanHome = home.isEmpty() ? QStringLiteral("/") : QDir::cleanPath(home);
    Locations l;
    const QString config = validRoot(configHome, cleanHome + QStringLiteral("/.config"));
    const QString state = validRoot(stateHome, cleanHome + QStringLiteral("/.local/state"));
    const QString cache = validRoot(cacheHome, cleanHome + QStringLiteral("/.cache"));
    QString runtime = validRoot(runtimeDir, QString());
    if (runtime.isEmpty())
        runtime = validRoot(tmpDir, QStringLiteral("/tmp"));

    l.configDir = config + QLatin1Char('/') + app;
    l.settingsFile = l.configDir + QLatin1Char('/') + QString::fromLatin1(settingsFileName);
    l.keymapFile = l.configDir + QLatin1Char('/') + QString::fromLatin1(keymapFileName);
    l.legacyConfigDir = config + QLatin1Char('/') + QString::fromLatin1(legacyDirName);
    l.stateDir = state + QLatin1Char('/') + app;
    l.recoveryDir = l.stateDir + QStringLiteral("/recovery");
    l.recordingsDir = l.stateDir + QStringLiteral("/recordings");
    l.cacheDir = cache + QLatin1Char('/') + app;
    l.runtimeDir = runtime + QLatin1Char('/') + app;
    return l;
}

Locations locations()
{
    return locationsFor(qEnvironmentVariable("XDG_CONFIG_HOME"),
                        qEnvironmentVariable("XDG_STATE_HOME"),
                        qEnvironmentVariable("XDG_CACHE_HOME"),
                        qEnvironmentVariable("XDG_RUNTIME_DIR"),
                        qEnvironmentVariable("TMPDIR"),
                        QDir::homePath());
}

QString Migration::summary() const
{
    if (!error.isEmpty())
        return QCoreApplication::translate("storage", "Settings from the earlier omawave folder could not be copied: %1 The originals are untouched.").arg(error);
    if (!copied.isEmpty())
        return QCoreApplication::translate("storage", "Settings were copied from the earlier omawave folder (%n file(s)). The originals are kept there and no longer read.", nullptr, copied.size());
    if (attempted && !kept.isEmpty())
        return QCoreApplication::translate("storage", "An earlier omawave settings folder still exists; OmaAudio's own settings take precedence and it is no longer read.");
    return {};
}

Migration migrateLegacyConfig(const Locations &where)
{
    Migration m;
    QDir legacy(where.legacyConfigDir);
    if (!legacy.exists())
        return m;
    m.attempted = true;
    // Once is once. Without a record of having done it, deleting a file here
    // to return to the defaults brought the old one back at the next launch.
    const QString marker = where.configDir + QStringLiteral("/.migrated-from-omawave");
    const bool done = QFileInfo::exists(marker);
    for (const char *name : {settingsFileName, keymapFileName}) {
        const QString source = where.legacyConfigDir + QLatin1Char('/') + QString::fromLatin1(name);
        const QString target = where.configDir + QLatin1Char('/') + QString::fromLatin1(name);
        if (!QFileInfo::exists(source))
            continue;
        if (done || QFileInfo::exists(target)) {
            m.kept.append(target);
            continue;
        }
        if (!QDir().mkpath(where.configDir)) {
            if (m.error.isEmpty())
                m.error = QCoreApplication::translate("storage", "%1 cannot be created.").arg(where.configDir);
            continue;
        }
        QFile file(source);
        if (!file.copy(target)) {
            if (m.error.isEmpty())
                m.error = QCoreApplication::translate("storage", "%1 could not be written: %2.").arg(target, file.errorString());
            continue;
        }
        m.copied.append(target);
    }
    if (!done && m.error.isEmpty() && QDir().mkpath(where.configDir)) {
        QFile record(marker);
        if (record.open(QIODevice::WriteOnly)) record.write("The earlier omawave settings were brought in once. Delete this file to do it again.\n");
    }
    return m;
}

void setLaunchMigration(const Migration &migration) { g_launchMigration = migration; }
const Migration &launchMigration() { return g_launchMigration; }

} // namespace omaaudio::storage
