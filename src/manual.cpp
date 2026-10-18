#include "manual.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace omaaudio::manual {

namespace {

bool readableFile(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.isReadable();
}

} // namespace

QString findManual()
{
    if (const QString explicitPath = qEnvironmentVariable("OMAAUDIO_MANUAL");
        !explicitPath.isEmpty() && readableFile(explicitPath))
        return QFileInfo(explicitPath).absoluteFilePath();

    const QString binary = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        binary + QStringLiteral("/../share/doc/omaaudio/manual.md"),
        // Prefer this checkout's guide over a different installed version.
        binary + QStringLiteral("/../docs/user-manual.md"),
        binary + QStringLiteral("/../../docs/user-manual.md"),
        QStringLiteral("/usr/share/doc/omaaudio/manual.md"),
        QStringLiteral("/usr/local/share/doc/omaaudio/manual.md"),
    };
    for (const QString &candidate : candidates)
        if (readableFile(candidate)) return QFileInfo(candidate).absoluteFilePath();
    return {};
}

QString readManual(const QString &path, QString *error)
{
    if (error) error->clear();
    if (path.isEmpty()) {
        if (error) *error = QCoreApplication::translate("manual",
            "The manual is not installed. Reinstall OmaAudio to restore Help › User Manual.");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QCoreApplication::translate("manual", "The manual could not be read: %1").arg(file.errorString());
        return {};
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    return stream.readAll();
}

QVector<Section> sections(const QString &markdown)
{
    QVector<Section> out;
    Section current;
    current.level = 1;
    QStringList body;
    bool inFence = false;      // a heading inside a fenced code block is code

    const auto flush = [&] {
        current.body = body.join(QLatin1Char('\n')).trimmed();
        if (!current.title.isEmpty() || !current.body.isEmpty()) out.push_back(current);
        body.clear();
    };

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("```"))) inFence = !inFence;
        int level = 0;
        if (!inFence) {
            while (level < line.size() && line.at(level) == QLatin1Char('#')) ++level;
            if (level < 1 || level > 3 || level >= line.size() || line.at(level) != QLatin1Char(' ')) level = 0;
        }
        if (level == 0) { body.push_back(line); continue; }
        flush();
        current.title = line.mid(level + 1).trimmed();
        current.level = level;
    }
    flush();
    return out;
}

} // namespace omaaudio::manual
