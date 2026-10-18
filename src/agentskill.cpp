#include "agentskill.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cstdio>

namespace omaaudio::skill {
namespace {
QStringList skillFolders()
{
    const auto executable = QCoreApplication::applicationDirPath();
    return {QDir::cleanPath(executable + "/../share/omaaudio/skills/omaaudio"),
            QDir::cleanPath(executable + "/../skills/omaaudio"),
            executable + "/skills/omaaudio",
            "/usr/share/omaaudio/skills/omaaudio",
            "/usr/local/share/omaaudio/skills/omaaudio"};
}

QStringList skillPlaces()
{
    const auto home = QDir::homePath();
    QStringList places{home + "/.agents/skills", home + "/.claude/skills"};
    const auto codex = qEnvironmentVariable("CODEX_HOME", home + "/.codex");
    if (!codex.isEmpty() && QFileInfo(codex).isDir())
        places.append(QDir(codex).absoluteFilePath("skills"));
    if (QFileInfo(home + "/.pi").isDir()) places.append(home + "/.pi/agent/skills");
    if (QFileInfo(home + "/.hermes").isDir()) places.append(home + "/.hermes/skills");
    const QDir profiles(home + "/.hermes/profiles");
    for (const auto &profile : profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        places.append(profiles.absoluteFilePath(profile + "/skills"));
    places.removeDuplicates();
    return places;
}

int reply(QJsonObject result, int code = 0)
{
    result.insert("ok", code == 0);
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    return code;
}
}

bool requested(const QStringList &arguments)
{
    return arguments.size() > 1 && arguments.at(1) == "skill";
}

int run(const QStringList &arguments)
{
    bool link = false, force = false, unlink = false;
    const auto usage = QStringLiteral("omaaudio skill [--link [--force] | --unlink]\n"
        "Inspect or link the packaged agent skill for the current user.\n"
        "--force moves a conflicting entry to a hidden backup before linking.\n"
        "--unlink removes only links pointing to this copy's skill.\n");
    for (const auto &argument : arguments.mid(2)) {
        if (argument == "--link") link = true;
        else if (argument == "--force") force = true;
        else if (argument == "--unlink") unlink = true;
        else if ((argument == "--help" || argument == "-h") && arguments.size() == 3) {
            const auto bytes = usage.toUtf8();
            std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
            return 0;
        } else return reply({{"error", "Unknown skill option: " + argument}, {"usage", usage}}, 2);
    }
    if ((force && !link) || (link && unlink))
        return reply({{"error", "Use --force with --link; --link and --unlink cannot be combined."}}, 2);

    const auto folders = skillFolders();
    QString folder;
    for (const auto &candidate : folders) {
        if (QFileInfo(candidate + "/SKILL.md").isFile()) {
            folder = QFileInfo(candidate).canonicalFilePath();
            break;
        }
    }
    if (folder.isEmpty() && !unlink)
        return reply({{"error", "The agent skill is missing. Reinstall OmaAudio, or keep skills/omaaudio beside the source build."}}, 1);

    QJsonArray places;
    QStringList trouble;
    int linked = 0, removed = 0;
    for (const auto &directory : skillPlaces()) {
        const auto target = directory + "/omaaudio";
        const QFileInfo existing(target);
        bool matches = existing.isSymLink() && !folder.isEmpty()
            && existing.canonicalFilePath() == folder;
        QJsonObject row{{"place", target}, {"linked", matches}};
        if (unlink) {
            // Also handle our dangling links after package content was removed.
            const bool ours = existing.isSymLink()
                && (matches || folders.contains(QDir::cleanPath(existing.symLinkTarget())));
            if (ours) {
                if (QFile::remove(target)) { row.insert("removed", true); matches = false; ++removed; }
                else trouble.append("Could not remove " + target);
            }
        } else if (link && !matches) {
            QString backup;
            if (existing.exists() || existing.isSymLink()) {
                if (!force) {
                    trouble.append(target + " already exists; --link --force keeps a backup and replaces it");
                    row.insert("note", "Existing entry preserved.");
                    places.append(row);
                    continue;
                }
                backup = directory + "/.omaaudio.backup-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!QDir().rename(target, backup)) {
                    trouble.append("Could not preserve " + target);
                    places.append(row);
                    continue;
                }
                row.insert("backup", backup);
            }
            if (QDir().mkpath(directory) && QFile::link(folder, target)) matches = true;
            else {
                trouble.append("Could not link " + target);
                if (!backup.isEmpty()) {
                    if (QDir().rename(backup, target)) row.remove("backup");
                    else trouble.append("Original entry remains at " + backup);
                }
            }
        }
        row.insert("linked", matches);
        if (matches) ++linked;
        places.append(row);
    }
    QJsonObject result{{"skill", folder.isEmpty() ? QString{} : folder + "/SKILL.md"},
                       {"places", places}, {"linked", linked}, {"removed", removed}};
    if (!trouble.isEmpty()) {
        result.insert("error", trouble.join("; "));
        return reply(result, 1);
    }
    result.insert("advice", unlink ? "Skill links removed; custom entries and backups are kept."
        : link ? "Start a new agent session to discover the OmaAudio skill."
               : "Run omaaudio skill --link to link the skill for this user.");
    return reply(result);
}
}
