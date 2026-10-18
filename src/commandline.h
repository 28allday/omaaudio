#pragma once

#include <QString>
#include <QStringList>

namespace omaaudio::cli {
inline constexpr auto version = "0.2.0-beta.1";
enum class Action { Run, Help, Version, Error };
struct Request {
    Action action = Action::Run;
    QString path;
    bool project = false;
    QString message;
};

// Called before GUI initialization, so information requests also work headlessly.
bool informationRequested(const QStringList &arguments);
Request parse(const QStringList &arguments);
} // namespace omaaudio::cli
