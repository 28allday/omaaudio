#include "commandline.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>

namespace omaaudio::cli {
bool informationRequested(const QStringList &arguments)
{
    bool information = false;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const auto &argument = arguments[i];
        if (argument == "--") break;
        if (argument == "--help" || argument == "--version") information = true;
        if (argument.startsWith('-') && argument.size() > 1 && !argument.startsWith("--")) {
            const bool compactInformation = std::all_of(argument.cbegin() + 1, argument.cend(),
                [](QChar c) { return c == 'h' || c == 'v'; });
            // A Qt option such as -qwindowtitle can consume the following --help
            // as its value. Let QGuiApplication parse these options first.
            if (argument.size() > 2 && !compactInformation) return false;
            information = information || compactInformation;
        }
    }
    return information;
}

Request parse(const QStringList &arguments)
{
    const auto error = [](const QString &message) {
        return Request{Action::Error, {}, false, message + "\nUse --help for usage."};
    };
    if (arguments.isEmpty()) return error("Missing application name.");
    QCommandLineParser parser;
    parser.setApplicationDescription("OmaAudio — audio editing, mixing and mastering.\nAgent command line: omaaudio cli --help\nAgent skill: omaaudio skill --link");
    parser.addOption({QStringList{"h", "help"}, "Show this help without starting the interface."});
    parser.addOption({QStringList{"v", "version"}, "Show the application version."});
    parser.addPositionalArgument("path", "One local audio file or project bundle directory.", "[path]");
    if (!parser.parse(arguments)) return error(parser.errorText());
    if (parser.isSet("help")) return {Action::Help, {}, false, parser.helpText()};
    if (parser.isSet("version")) return {Action::Version, {}, false, QStringLiteral("OmaAudio ") + QString::fromLatin1(version)};
    const auto files = parser.positionalArguments();
    if (files.size() > 1) return error("Open one audio file or project bundle at a time.");
    if (files.isEmpty()) return {};
    QString path = files.first();
    if (path.isEmpty()) return error("Choose a local audio file or project bundle.");
    const QUrl url(path, QUrl::StrictMode);
    if (path.startsWith("file:", Qt::CaseInsensitive)) {
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host().compare("localhost", Qt::CaseInsensitive) != 0)
            || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment())
            return error("Use a valid local file URL without a query or fragment.");
        auto local = url; local.setHost({}); path = local.toLocalFile();
        if (path.isEmpty()) return error("The file URL has no path.");
    } else if (!url.scheme().isEmpty() && path.contains("://")) {
        return error("Only local audio files and project bundles can be opened.");
    }
    if (path.contains(QChar::Null)) return error("File paths cannot contain NUL characters.");
    const QFileInfo file(path);
    // Lexical cleanup of link/../file changes which file a symlink resolves to.
    // Keep user path components and let the filesystem resolve them on opening.
    const auto absolute = QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path;
    return {Action::Run, absolute, file.isDir(), {}};
}
} // namespace omaaudio::cli
