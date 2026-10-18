#pragma once

#include <QString>
#include <QStringList>

// Where OmaAudio keeps things, by XDG role, under one application-owned name.
// Pure functions over the environment so the tests can hand them any roots.
//
//   configuration  $XDG_CONFIG_HOME/omaaudio/{omaaudio.conf,keymap.toml}
//   state          $XDG_STATE_HOME/omaaudio/{recovery,recordings}
//   caches         $XDG_CACHE_HOME/omaaudio            (disposable; engine scratch, display tiles)
//   runtime        $XDG_RUNTIME_DIR/omaaudio           (this login only; textures, sockets)
//
// The application was first shipped under the organisation name "omawave", so
// its settings and keymap lived in $XDG_CONFIG_HOME/omawave. migrateLegacyConfig
// copies those into place once, keeps the originals, and never overwrites a file
// OmaAudio has since written itself.
namespace omaaudio::storage {

inline constexpr const char *appDirName = "omaaudio";
inline constexpr const char *legacyDirName = "omawave";
inline constexpr const char *settingsFileName = "omaaudio.conf";
inline constexpr const char *keymapFileName = "keymap.toml";

struct Locations {
    QString configDir;        // .../omaaudio under the config home
    QString settingsFile;     // configDir/omaaudio.conf
    QString keymapFile;       // configDir/keymap.toml
    QString legacyConfigDir;  // .../omawave under the config home
    QString stateDir;         // .../omaaudio under the state home
    QString recoveryDir;      // stateDir/recovery
    QString recordingsDir;    // stateDir/recordings
    QString cacheDir;         // .../omaaudio under the cache home
    QString runtimeDir;       // .../omaaudio under the runtime dir, TMPDIR or /tmp
};

// Resolve from explicit roots. An XDG value that is empty or not an absolute
// path is invalid by the specification and falls back to the home default.
Locations locationsFor(const QString &configHome, const QString &stateHome,
                       const QString &cacheHome, const QString &runtimeDir,
                       const QString &tmpDir, const QString &home);

// Resolve from the process environment now.
Locations locations();

struct Migration {
    bool attempted = false;   // a legacy directory existed
    QStringList copied;       // destination paths written this call
    QStringList kept;         // destinations that already existed and were left alone
    QString error;            // first failure, empty when none
    // One sentence for a person, empty when nothing happened.
    QString summary() const;
};

// Copy omaaudio.conf and keymap.toml from the legacy directory when the new
// directory lacks them. Repeatable; never deletes or rewrites the originals.
Migration migrateLegacyConfig(const Locations &where);

// The migration performed at this launch, for the Preferences panel.
void setLaunchMigration(const Migration &migration);
const Migration &launchMigration();

} // namespace omaaudio::storage
