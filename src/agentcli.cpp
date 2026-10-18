#include "agentcli.h"
#include "backend.h"
#include "commandline.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QFile>
#include <QScopeGuard>
#include <QQueue>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <limits>

namespace omaaudio::agent {
namespace {
constexpr qsizetype maxInput = 1024 * 1024;
constexpr int maxRequests = 10000;
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int signal) { interrupted = signal; }

QJsonObject failure(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

const QMap<QString, QString> &aliases()
{
    static const QMap<QString, QString> map{
        {"project.new", "newProject"}, {"project.open", "loadProject"}, {"project.save", "saveAs"},
        {"audio.open", "open"}, {"audio.import", "importFile"}, {"aaf.import", "importSession"},
        {"track.add", "addTrack"}, {"track.remove", "removeTrack"}, {"track.rename", "renameTrack"},
        {"track.duplicate", "duplicateTrack"}, {"track.gain", "setTrackGainDb"}, {"track.pan", "setTrackPan"},
        {"track.mute", "setTrackMute"}, {"track.solo", "setTrackSolo"}, {"track.focus", "focusTrack"},
        {"track.routing", "setTrackRouting"}, {"clip.properties", "clipProperties"},
        {"clip.envelope", "clipEnvelopeTarget"}, {"clip.envelope.edit", "editClipEnvelope"},
        {"clip.effects", "clipEffectsTarget"}, {"clip.effect.add", "addClipEffect"},
        {"clip.effect.remove", "removeClipEffect"}, {"clip.effect.move", "moveClipEffect"},
        {"clip.effect.bypass", "setClipEffectBypass"}, {"clip.effect.control", "setClipEffectControl"},
        {"clip.effect.automation", "editClipEffectAutomation"}, {"clip.effects.clear", "clearClipEffects"},
        {"clip.effects.bake", "bakeClipEffects"}, {"clips.effect.add", "addEffectToSelectedClips"},
        {"clip.apply", "applyClipProperties"}, {"clip.split", "split"}, {"clip.stretch", "stretchFocusedClip"},
        {"clip.gain", "setClipGainDb"}, {"clip.fades", "setClipFades"},
        {"clip.trimLimits", "clipTrimLimits"}, {"clip.trim", "trimClip"},
        {"clips.apply", "applySelectedClipProperties"},
        {"clips.stretchTarget", "selectedStretchTarget"}, {"clips.stretch", "stretchSelectedClips"},
        {"selection.set", "select"}, {"selection.all", "selectAll"}, {"selection.clear", "clearSelection"},
        {"selection.tracks", "setSelectedTracks"}, {"selection.clips", "setSelectedClips"}, {"clips.move", "moveSelectedClips"},
        {"group.create", "createClipGroup"}, {"group.update", "updateClipGroup"},
        {"group.detach", "detachSelectedClips"}, {"group.dissolve", "dissolveClipGroup"}, {"group.link", "setClipGroupsLinked"},
        {"transport.play", "play"}, {"transport.stop", "stop"}, {"transport.seek", "seekFrames"},
        {"transport.shuttle", "shuttle"},
        {"record.start", "startRecording"}, {"record.stop", "stopRecording"},
        {"plugin.list", "listPlugins"}, {"plugin.add", "addPlugin"}, {"plugin.remove", "removePlugin"},
        {"plugin.ports", "requestPluginPorts"}, {"plugin.control", "setPluginControl"},
        {"plugin.kind", "pluginKind"}, {"plugin.response", "pluginResponse"},
        {"preset.list", "pluginPresetContext"}, {"preset.apply", "applyPluginPreset"},
        {"preset.clipList", "clipEffectPresetContext"}, {"preset.clipApply", "applyClipEffectPreset"},
        {"plugin.bypass", "setPluginBypass"}, {"plugin.window", "openPluginEditor"}, {"plugin.windowClose", "closePluginEditor"}, {"automation.edit", "editAutomation"},
        {"ducking.analyse", "analyseDucking"}, {"ducking.apply", "applyGeneratedDucking"}, {"ducking.cancel", "cancelDucking"},
        {"ducking.state", "generatedDuckingState"}, {"ducking.unfollow", "stopFollowingDucking"},
        {"ducking.remove", "removeGeneratedDucking"},
        {"effect.preview", "beginEffectPreview"}, {"effect.apply", "applyEffectPreview"}, {"effect.cancel", "cancelEffectPreview"},
        {"sound.get", "soundSettings"}, {"sound.set", "applySound"}, {"sound.roomEcho", "reduceRoomEcho"},
        {"sound.choices", "soundChoices"}, {"sound.choice", "setSoundChoice"},
        {"effect.repeat", "repeatLastEffect"},
        {"pauses.find", "findPauses"}, {"pauses.keep", "setPauseKept"}, {"pauses.plan", "pausePlan"},
        {"pauses.apply", "applyPauses"}, {"pauses.clear", "clearPauses"},
        {"recipe.get", "recipe"}, {"recipe.kinds", "recipeStepKinds"}, {"recipe.effectStep", "effectStep"},
        {"recipe.save", "saveRecipe"}, {"recipe.duplicate", "duplicateRecipe"}, {"recipe.rename", "renameRecipe"},
        {"recipe.remove", "removeRecipe"}, {"recipe.target", "recipeTarget"}, {"recipe.run", "runRecipe"},
        {"recipe.stop", "stopRecipe"}, {"recipe.again", "runLastRecipeAgain"}, {"recipe.clear", "clearRecipeRun"},
        {"marker.add", "addMarker"}, {"marker.range", "addRangeMarker"}, {"marker.remove", "removeMarker"},
        {"master.trim", "setMasterTrimDb"}, {"master.analyse", "analyseMaster"},
        {"analyse", "analyseAudio"}, {"generate", "generateAudio"}, {"clips.matchLoudness", "matchClipLoudness"},
        {"speech.target", "speechAlignmentTarget"}, {"speech.align", "alignSpeech"}, {"speech.cancel", "cancelSpeechAlignment"},
        {"remix.target", "remixTarget"}, {"performance.load", "audioLoad"}, {"performance.check", "checkComputer"}, {"remix", "remixClip"}, {"remix.cancel", "cancelRemix"},
        {"spectral.heal", "healSpectral"}, {"spectral.denoise", "denoiseSpectral"},
        {"video.import", "importVideo"}, {"video.offset", "setVideoOffset"},
        {"export", "queueRender"}, {"batch.preview", "previewFileBatch"}, {"batch.process", "queueFileBatch"},
        {"project.collect", "collectProject"}, {"project.cancelCollection", "cancelCollection"},
        {"batch.cancel", "cancelFileBatch"}, {"batch.cancelFile", "cancelBatchFile"}, {"batch.retry", "retryBatchFile"},
        {"batch.resume", "resumeBatchFile"}, {"batch.resumeAll", "resumeFileBatch"},
        {"batch.reset", "resetFileBatchQueue"}
    };
    return map;
}

bool cancellation(const QString& op)
{
    const auto name = aliases().value(op, op);
    return op == "cancel" || name == "cancelDucking" || name == "cancelCollection" || name == "cancelFileBatch" || name == "cancelBatchFile";
}

bool observation(const QString& op)
{
    return op == "get" || op == "inspect" || op == "schema" || cancellation(op);
}

bool stopping(const QString& op)
{
    const auto name = aliases().value(op, op);
    return name == "stop" || name == "stopRecording";
}

bool immediateStop(const QJsonObject& request)
{
    return !request.value("wait").toBool(true) && stopping(request.value("op").toString());
}

QString wireType(QMetaType type)
{
    switch (type.id()) {
    case QMetaType::Void: return "null";
    case QMetaType::Bool: return "boolean";
    case QMetaType::Int: case QMetaType::UInt: return "integer";
    case QMetaType::LongLong: case QMetaType::ULongLong: return "integer-or-decimal-string";
    case QMetaType::Double: case QMetaType::Float: return "number";
    case QMetaType::QString: case QMetaType::QUrl: case QMetaType::QColor: case QMetaType::QFont: return "string";
    case QMetaType::QVariantMap: return "object";
    case QMetaType::QVariantList: case QMetaType::QStringList: return "array";
    default: return {};
    }
}

bool convert(const QJsonValue &input, QMetaType type, QVariant &output)
{
    const auto id = type.id();
    if (id == QMetaType::QVariantMap && input.isObject()) output = input.toObject().toVariantMap();
    else if (id == QMetaType::QVariantList && input.isArray()) output = input.toArray().toVariantList();
    else if (id == QMetaType::QStringList && input.isArray()) {
        QStringList strings;
        for (const auto &item : input.toArray()) { if (!item.isString()) return false; strings.append(item.toString()); }
        output = strings;
    } else if (id == QMetaType::Bool && input.isBool()) output = input.toBool();
    else if (id == QMetaType::QString && input.isString() && !input.toString().contains(QChar::Null)) output = input.toString();
    else if (id == QMetaType::QUrl && input.isString()) {
        const QUrl url(input.toString(), QUrl::StrictMode); if (!url.isValid()) return false; output = url;
    } else if (id == QMetaType::QColor && input.isString()) {
        const QColor color(input.toString()); if (!color.isValid()) return false; output = color;
    } else if (id == QMetaType::QFont && input.isString()) {
        QFont font; if (!font.fromString(input.toString())) return false; output = font;
    } else if (id == QMetaType::Int || id == QMetaType::UInt || id == QMetaType::LongLong || id == QMetaType::ULongLong) {
        if (input.isString() && (id == QMetaType::LongLong || id == QMetaType::ULongLong)) {
            bool ok = false;
            const QString text = input.toString();
            if (text.isEmpty() || text.trimmed() != text) return false;
            if (id == QMetaType::LongLong) output = text.toLongLong(&ok);
            else { if (text.startsWith('-')) return false; output = text.toULongLong(&ok); }
            if (!ok) return false;
        } else {
            if (!input.isDouble()) return false;
            const double number = input.toDouble();
            // JSON clients commonly use IEEE doubles; larger integers must be strings.
            if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > 9007199254740991.0) return false;
            if (id == QMetaType::Int && (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())) return false;
            if (id == QMetaType::UInt && (number < 0 || number > std::numeric_limits<uint>::max())) return false;
            if (id == QMetaType::ULongLong && number < 0) return false;
            output = QVariant::fromValue(static_cast<qlonglong>(number));
        }
        if (!output.convert(type)) return false;
    } else if ((id == QMetaType::Double || id == QMetaType::Float) && input.isDouble()) {
        const double number = input.toDouble();
        if (!std::isfinite(number) || (id == QMetaType::Float && std::abs(number) > std::numeric_limits<float>::max())) return false;
        output = number; if (!output.convert(type)) return false;
    } else return false;
    return true;
}

bool exposed(const QMetaMethod &method)
{
    return method.access() == QMetaMethod::Public && method.methodType() == QMetaMethod::Method;
}

QJsonObject schema()
{
    const auto &meta = Backend::staticMetaObject;
    QJsonArray methods, properties;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const auto method = meta.method(i);
        if (!exposed(method)) continue;
        QJsonArray parameters;
        const auto names = method.parameterNames();
        for (int p = 0; p < method.parameterCount(); ++p)
            parameters.append(QJsonObject{{"name", QString::fromLatin1(names[p])},
                {"type", wireType(method.parameterMetaType(p))}, {"qtType", QString::fromLatin1(method.parameterTypes()[p])}});
        if (method.name() == "setTrackGainDb" || method.name() == "gainSelection") {
            for (int p = 0; p < parameters.size(); ++p) {
                auto parameter = parameters[p].toObject();
                if (parameter.value("name") == "db") { parameter["minimum"] = -80; parameter["maximum"] = 48; parameters[p] = parameter; }
            }
        }
        methods.append(QJsonObject{{"op", QString::fromLatin1(method.name())},
            {"signature", QString::fromLatin1(method.methodSignature())}, {"args", parameters},
            {"returns", wireType(method.returnMetaType())}, {"readOnly", method.isConst()},
            {"desktopOnly", method.name() == "revealRender"}});
    }
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const auto property = meta.property(i);
        properties.append(QJsonObject{{"name", QString::fromLatin1(property.name())},
            {"type", wireType(property.metaType())}, {"writable", property.isWritable()}});
    }
    QJsonObject alias;
    for (auto it = aliases().cbegin(); it != aliases().cend(); ++it) alias[it.key()] = it.value();
    return {{"protocol", "omaaudio-cli"}, {"version", 1}, {"applicationVersion", cli::version},
        {"methods", methods}, {"properties", properties}, {"aliases", alias},
        {"builtins", QJsonArray{"schema", "inspect", "get", "set", "wait", "cancel", "quit"}},
        {"units", "All timeline positions and lengths are project sample frames unless named seconds, Hz, dB or ms."},
        {"defaults", "Each signature is callable. Shorter signatures omit trailing arguments using the application's defaults."}};
}

class Session final : public QObject {
public:
    Session(Backend &backend, int output, int timeout, bool audio, bool events, bool discard, QString savePath)
        : m_backend(backend), m_output(output), m_timeout(timeout), m_audio(audio), m_events(events),
          m_discard(discard), m_savePath(std::move(savePath))
    {
        connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
        connect(&backend, &Backend::lastErrorChanged, this, [this] {
            if (!m_backend.lastError().isEmpty()) {
                // An undispatched wait owns no operation yet. A job accepted
                // with wait:false can finish while this request is waiting for
                // idle; its error remains a background event, not a failed wait.
                const bool settlingEarlierWork = m_active && !m_dispatched
                    && m_request.value("op").toString() == "wait";
                if (!settlingEarlierWork) m_errors.append(m_backend.lastError());
                if ((!m_active || settlingEarlierWork) && !m_finishing) {
                    if (!m_exitCode) m_exitCode = 1;
                    send({{"type", "error"}, {"ok", false}, {"error", QJsonObject{{"code", "background_failed"},
                        {"message", m_backend.lastError()}}}});
                }
            }
        });
        connect(&backend, &Backend::pathNeeded, this, [this] { m_errors.append("Saving needs a path. Use saveAs with a path."); });
        connect(&backend, &Backend::importWarning, this, [this](const QString &text) { m_warnings.append(text); });
        m_timer.start(10);
    }

    void send(QJsonObject result)
    {
        const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::write(m_output, bytes.constData() + offset, static_cast<size_t>(bytes.size() - offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { m_exitCode = 1; m_eof = true; m_queue.clear(); m_backend.cancelCommandWork(); break; }
            offset += n;
        }
    }

    QJsonObject state()
    {
        QJsonObject result;
        for (const char *name : {"projectPath", "dirty", "sampleRate", "takeFrames", "trackCount", "focusedTrack",
                "selectionStart", "selectionEnd", "selectedTracks", "selectedClips", "playhead", "playing", "recording", "canUndo", "canRedo"})
            result[name] = QJsonValue::fromVariant(m_backend.property(name));
        result["busy"] = m_backend.commandBusy();
        return result;
    }

    void enqueue(const QJsonObject &request)
    {
        if (m_queue.size() >= maxRequests) {
            send(failure("input_limit", "Too many queued requests.")); m_exitCode = 2; m_eof = true; return;
        }
        const QString op = request.value("op").toString();
        // Cancellation and observation remain available while a waited-for job runs.
        if (m_active && (observation(op) || (m_dispatched && immediateStop(request)))) {
            immediate(request); return;
        }
        m_queue.enqueue(request);
    }

    void initialLoad(const QString &path) { m_initialLoad = true; enqueue({{"op", "loadProject"}, {"args", QJsonObject{{"path", path}}}, {"id", "open"}}); }
    void input(bool stream) { m_stream = stream; }
    void endInput() { m_eof = true; }
    void failInput(const QString &message) { send(failure("invalid_request", message)); m_exitCode = 2; m_eof = true; m_queue.clear(); }
    int exitCode() const { return m_exitCode; }

private:
    QJsonObject inspect()
    {
        QJsonObject result;
        const auto *meta = m_backend.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            result[QString::fromLatin1(property.name())] = QJsonValue::fromVariant(property.read(&m_backend));
        }
        result["projectMetadata"] = QJsonObject::fromVariantMap(m_backend.projectMetadata());
        result["busy"] = m_backend.commandBusy();
        return result;
    }

    QJsonValue resolve(const QJsonValue &value, QString &error, int depth = 0)
    {
        if (depth > 64) { error = "Arguments exceed the nesting limit."; return {}; }
        if (value.isObject()) {
            auto object = value.toObject();
            if (object.size() == 1 && object.contains("$ref")) {
                if (!object["$ref"].isString()) { error = "$ref must be a string."; return {}; }
                const auto path = object["$ref"].toString().split('.');
                QJsonValue found = m_results.value(path[0]);
                for (int i = 1; i < path.size(); ++i) {
                    if (found.isObject()) found = found.toObject().value(path[i]);
                    else if (found.isArray()) {
                        bool ok = false; const int index = path[i].toInt(&ok); const auto list = found.toArray();
                        found = ok && index >= 0 && index < list.size() ? list[index] : QJsonValue(QJsonValue::Undefined);
                    } else found = QJsonValue(QJsonValue::Undefined);
                }
                if (found.isUndefined()) error = "Unknown result reference: " + object["$ref"].toString();
                return found;
            }
            const auto keys = object.keys();
            for (const auto &key : keys) {
                const auto resolved = resolve(object.value(key), error, depth + 1);
                if (!error.isEmpty()) return {};
                object[key] = resolved;
            }
            return object;
        }
        if (value.isArray()) {
            QJsonArray result; for (const auto &item : value.toArray()) result.append(resolve(item, error, depth + 1));
            return result;
        }
        return value;
    }

    QJsonObject dispatch(const QJsonObject &request)
    {
        const QSet<QString> keys{"id", "op", "args", "wait", "timeoutMs", "discardChanges"};
        for (auto it = request.begin(); it != request.end(); ++it)
            if (!keys.contains(it.key())) return failure("invalid_request", "Unknown request field: " + it.key());
        if (!request.value("op").isString() || request.value("op").toString().isEmpty())
            return failure("invalid_request", "A request needs a non-empty op string.");
        if (request.contains("wait") && !request.value("wait").isBool()) return failure("invalid_request", "wait must be boolean.");
        if (request.contains("discardChanges") && !request.value("discardChanges").isBool())
            return failure("invalid_request", "discardChanges must be boolean.");
        if (request.contains("id") && !request.value("id").isString() && !request.value("id").isDouble())
            return failure("invalid_request", "id must be a string or a number.");
        QString referenceError;
        QString op = request.value("op").toString();
        const QJsonValue args = resolve(request.value("args").isUndefined() ? QJsonObject{} : request.value("args"), referenceError);
        if (!referenceError.isEmpty()) return failure("invalid_reference", referenceError);
        if (!args.isObject() && !args.isArray()) return failure("invalid_arguments", "args must be an object or an array.");
        if (op == "wait") {
            if (!args.isObject()) return failure("invalid_arguments", "wait takes {ms: integer, until: idle|stopped|recording}, with both fields optional.");
            const auto object = args.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
                if (it.key() != "ms" && it.key() != "until") return failure("invalid_arguments", "Unknown wait field: " + it.key());
            QVariant duration;
            if (object.contains("ms") && (!convert(object.value("ms"), QMetaType::fromType<int>(), duration) || duration.toInt() < 0))
                return failure("invalid_arguments", "wait ms must be a non-negative integer.");
            m_waitMs = duration.toInt(); m_waitUntil = object.value("until").toString("idle");
            if ((object.contains("until") && !object.value("until").isString()) || !QStringList{"idle", "stopped", "recording"}.contains(m_waitUntil))
                return failure("invalid_arguments", "wait until must be idle, stopped or recording.");
            return {{"ok", true}, {"result", QJsonValue::Null}};
        }
        if (op == "schema" || op == "inspect" || op == "cancel" || op == "quit") {
            if ((args.isObject() && !args.toObject().isEmpty()) || (args.isArray() && !args.toArray().isEmpty()))
                return failure("invalid_arguments", op + " takes no arguments.");
            if (op == "schema") return {{"ok", true}, {"result", schema()}};
            if (op == "inspect") return {{"ok", true}, {"result", inspect()}};
            if (op == "cancel") m_backend.cancelCommandWork();
            if (op == "quit") {
                if (m_backend.dirty() && !m_discard && m_savePath.isEmpty())
                    return failure("unsaved_changes", "Save the project before quit, or launch with --discard to allow unsaved changes.");
                m_eof = true; m_stream = false; m_queue.clear();
            }
            return {{"ok", true}, {"result", QJsonValue::Null}};
        }
        if (op == "get" || op == "set") {
            const auto object = args.toObject();
            if (!args.isObject() || !object.value("property").isString() || object.size() != (op == "get" ? 1 : 2)
                || (op == "set" && !object.contains("value")))
                return failure("invalid_arguments", "Use {\"property\":\"name\"} and, for set, a value.");
            const auto name = object.value("property").toString().toLatin1();
            const auto *meta = m_backend.metaObject(); const int index = meta->indexOfProperty(name);
            if (index < meta->propertyOffset()) return failure("unknown_property", "Unknown property: " + QString::fromLatin1(name));
            const auto property = meta->property(index);
            if (op == "set") {
                if (!property.isWritable()) return failure("read_only", "Property is read-only: " + QString::fromLatin1(name));
                QVariant value;
                if (!convert(object.value("value"), property.metaType(), value))
                    return failure("invalid_arguments", "Wrong value type; expected " + wireType(property.metaType()));
                if (name == "recordMonitor" && value.toBool() && !m_audio)
                    return failure("audio_disabled", "Monitoring requires --audio.");
                if (!property.write(&m_backend, value)) return failure("operation_failed", "Could not set property.");
            }
            return {{"ok", true}, {"result", QJsonValue::fromVariant(property.read(&m_backend))}};
        }
        op = aliases().value(op, op);
        if ((op == "newProject" || op == "open" || op == "openFile" || op == "loadProject" || op == "recoverSession")
                && m_backend.dirty() && !request.value("discardChanges").toBool())
            return failure("unsaved_changes", "Save the current session before replacing it, or explicitly pass discardChanges:true.");
        if (op == "revealRender") return failure("desktop_only", "Use renderById to obtain the exported file path; opening a desktop file manager is not available in CLI mode.");
        if (!m_audio && (op == "play" || op == "togglePlay" || op == "shuttle" || op == "startRecording"
                || (op == "setRecordMonitor" && (args.isArray() ? (!args.toArray().isEmpty() && args.toArray()[0].toBool()) : args.toObject().value("enabled").toBool()))))
            return failure("audio_disabled", "Playback, recording and monitoring require --audio.");
        const auto *meta = m_backend.metaObject();
        bool found = false;
        for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
            const auto method = meta->method(i);
            if (!exposed(method) || QString::fromLatin1(method.name()) != op) continue;
            found = true;
            const int count = method.parameterCount();
            if (count != (args.isArray() ? args.toArray().size() : args.toObject().size())) continue;
            QList<QVariant> values; values.reserve(count);
            const auto names = method.parameterNames();
            bool valid = true;
            for (int p = 0; p < count; ++p) {
                QVariant value;
                const auto argument = args.isArray() ? args.toArray()[p] : args.toObject().value(QString::fromLatin1(names[p]));
                if (!convert(argument, method.parameterMetaType(p), value)) { valid = false; break; }
                // GUI controls cannot select a nonexistent track. Enforce the same invariant for agents.
                if (names[p] == "track" && value.toInt() >= 0 && m_backend.trackIndexById(value.toInt()) < 0)
                    return failure("invalid_target", "No track with id " + value.toString());
                values.append(std::move(value));
            }
            if (!valid) continue;
            QVariantMap named;
            for (int p = 0; p < count; ++p) named[QString::fromLatin1(names[p])] = values[p];
            if ((op == "setTrackGainDb" || op == "gainSelection")
                && (named.value("db").toDouble() < -80 || named.value("db").toDouble() > 48))
                return failure("invalid_arguments", "Gain must be between -80 and +48 dB.");
            if (op == "queueRender") {
                const auto spec = named.value("spec").toMap();
                static const QMap<QString, QMetaType> types{
                    {"path", QMetaType::fromType<QString>()}, {"format", QMetaType::fromType<QString>()},
                    {"bitDepth", QMetaType::fromType<int>()}, {"sampleRate", QMetaType::fromType<int>()},
                    {"channels", QMetaType::fromType<int>()}, {"dither", QMetaType::fromType<bool>()},
                    {"normalise", QMetaType::fromType<QString>()}, {"targetLufs", QMetaType::fromType<double>()},
                    {"ceilingDbTp", QMetaType::fromType<double>()}, {"peakTargetDb", QMetaType::fromType<double>()},
                    {"name", QMetaType::fromType<QString>()}, {"bitrateKbps", QMetaType::fromType<int>()},
                    {"markers", QMetaType::fromType<QString>()}, {"includeMetadata", QMetaType::fromType<bool>()},
                    {"stems", QMetaType::fromType<bool>()}, {"tracks", QMetaType::fromType<QVariantList>()},
                    {"start", QMetaType::fromType<qint64>()}, {"end", QMetaType::fromType<qint64>()},
                    {"range", QMetaType::fromType<QString>()}};
                for (auto field = spec.begin(); field != spec.end(); ++field) {
                    QVariant converted;
                    if (!types.contains(field.key()) || !convert(QJsonValue::fromVariant(field.value()), types.value(field.key()), converted))
                        return failure("invalid_arguments", "Unknown or incorrectly typed export field: " + field.key());
                }
                if (spec.contains("normalise") && !QStringList{"none", "peak", "loudness"}.contains(spec.value("normalise").toString()))
                    return failure("invalid_arguments", "normalise must be none, peak or loudness.");
                if (spec.contains("range") && !QStringList{"project", "selection"}.contains(spec.value("range").toString()))
                    return failure("invalid_arguments", "range must be project or selection.");
                if (spec.contains("range") && (spec.contains("start") || spec.contains("end")))
                    return failure("invalid_arguments", "Give either range or start and end, not both.");
                // "tracks" chooses the stems: existing track ids, played even when muted.
                if (spec.contains("tracks")) {
                    if (!spec.value("stems").toBool())
                        return failure("invalid_arguments", "tracks chooses stems: add \"stems\":true.");
                    const auto chosen = QJsonValue::fromVariant(spec.value("tracks")).toArray();
                    if (chosen.isEmpty()) return failure("invalid_arguments", "tracks must list at least one track id.");
                    for (const auto &id : chosen)
                        if (!id.isDouble() || id.toDouble() != std::floor(id.toDouble()) || m_backend.trackIndexById(id.toInt()) < 0)
                            return failure("invalid_target", "tracks must list existing track ids.");
                }
            }
            const QSet<QString> trackOnly{"setTrackGainDb", "setTrackPan", "setTrackMute", "setTrackSolo", "removeTrack",
                "renameTrack", "moveTrack", "focusTrack", "toggleMute", "toggleSolo", "setTrackRole", "setTrackColor",
                "setTrackHeight", "stepTrackHeight", "duplicateTrack", "beginClipMove", "beginClipSlip", "previewClipMove"};
            if (trackOnly.contains(op) && m_backend.trackIndexById(named.value("track").toInt()) < 0)
                return failure("invalid_target", "This operation requires an existing track id.");
            const QSet<QString> pluginOps{"removePlugin", "movePlugin", "setPluginBypass", "setPluginControl", "requestPluginPorts"};
            if (pluginOps.contains(op)) {
                const int target = named.value("track", -1).toInt();
                const auto inserts = target == Backend::kMasterTarget ? m_backend.masterInserts()
                    : m_backend.trackById(target == -1 ? m_backend.focusedTrack() : target).value("inserts").toList();
                const int slot = named.value(op == "movePlugin" ? "from" : "slot").toInt();
                if (target < -2 || slot < 0 || slot >= inserts.size())
                    return failure("invalid_target", "No plugin at that track and slot.");
                if (op == "movePlugin" && (named.value("to").toInt() < 0 || named.value("to").toInt() >= inserts.size()))
                    return failure("invalid_target", "Plugin destination slot does not exist.");
                if (op == "setPluginControl") {
                    const int resolvedTarget = target == -1 ? m_backend.focusedTrack() : target;
                    if (!m_backend.pluginPortsComplete() || m_backend.pluginPortsSlot() != slot || m_backend.pluginPortsTarget() != resolvedTarget)
                        m_backend.requestPluginPorts(slot, target);
                    bool control = false;
                    for (const auto &entry : m_backend.pluginPorts()) {
                        const auto port = entry.toMap();
                        if (port.value("index").toLongLong() != named.value("port").toLongLong()) continue;
                        control = true;
                        const double value = named.value("value").toDouble();
                        if (value < port.value("min").toDouble() || value > port.value("max").toDouble()
                            || (port.value("integer").toBool() && value != std::trunc(value))
                            || (port.value("toggled").toBool() && value != 0 && value != 1))
                            return failure("invalid_arguments", "Plugin value is outside its declared range or step. Read pluginPorts.");
                    }
                    if (!control) return failure("invalid_target", "No writable plugin control with that port index.");
                }
            }
            if (op == "play" || (op == "shuttle" && !m_backend.playing()) || (op == "togglePlay" && !m_backend.playing() && !m_backend.recording())) {
                if (m_backend.takeFrames() <= 0) return failure("empty_session", "Open or import audio before playback.");
                if (m_backend.recording()) return failure("operation_rejected", "Stop recording before starting playback.");
                m_waitForPlay = true; m_playEpoch = m_backend.commandPlaybackEpoch();
            }
            if (op == "stop" || (op == "togglePlay" && (m_backend.playing() || m_backend.recording()))) m_waitForStop = true;
            QSet<int> batchJobs;
            if (op == "resumeFileBatch") {
                for (const auto& value : m_backend.fileBatchJobs()) {
                    const auto job = value.toMap();
                    if (job.value("state") == "paused") batchJobs.insert(job.value("id").toInt());
                }
            } else if (op == "retryBatchFile" || op == "resumeBatchFile") batchJobs.insert(named.value("id").toInt());
            QVariant returned = method.returnType() == QMetaType::Void ? QVariant{} : QVariant(method.returnMetaType());
            QList<void *> pointers; pointers.reserve(count + 1);
            pointers.append(method.returnType() == QMetaType::Void ? nullptr : returned.data());
            for (auto &value : values) pointers.append(value.data());
            // Unlike the ten-argument invokeMethod convenience overload, metacall
            // supports the full harmonic-mask operation and moc's default-argument clones.
            QMetaObject::metacall(&m_backend, QMetaObject::InvokeMetaMethod, i, pointers.data());
            if (!batchJobs.isEmpty() && returned.toBool()) m_batchJobs = std::move(batchJobs);
            if (returned.metaType().id() == QMetaType::Bool && !returned.toBool() && !method.isConst() && m_errors.isEmpty())
                return failure("operation_rejected", "The operation was not accepted. Inspect the session and its target/stamp before retrying.");
            return {{"ok", true}, {"result", method.returnType() == QMetaType::Void ? QJsonValue(QJsonValue::Null) : QJsonValue::fromVariant(returned)}};
        }
        return failure(found ? "invalid_arguments" : "unknown_operation", found
            ? "Arguments do not match a signature. Use schema to see names, types and default-argument forms for " + op
            : "Unknown operation: " + op);
    }

    void remember(QJsonObject &response)
    {
        if (!response.value("ok").toBool() || !response.contains("id")) return;
        const auto key = response.value("id").toVariant().toString();
        if (m_results.contains(key)) {
            m_resultBytes -= QJsonDocument(m_results.take(key).toObject()).toJson(QJsonDocument::Compact).size();
        }
        response["referenceStored"] = true;
        const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact).size();
        if (m_results.size() < maxRequests && bytes + m_resultBytes <= 32 * 1024 * 1024) {
            m_results[key] = response; m_resultBytes += bytes;
        } else response["referenceStored"] = false;
    }

    void immediate(const QJsonObject &request)
    {
        const auto previous = m_errors; m_errors.clear();
        const bool previousPlay = m_waitForPlay, previousStop = m_waitForStop;
        auto response = dispatch(request);
        m_waitForPlay = previousPlay; m_waitForStop = previousStop;
        if (!m_errors.isEmpty()) response = failure("operation_failed", m_errors.join('\n'));
        m_errors = previous;
        response["id"] = request.value("id"); response["type"] = "response"; response["state"] = state();
        if (!response.value("ok").toBool() && !m_exitCode) m_exitCode = 1;
        remember(response);
        send(response);
        if (request.value("op") == "cancel" && response.value("ok").toBool()) m_cancelled = true;
    }

    void readInput()
    {
        if (!m_stream || m_eof || m_initialLoad) return;
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&fd, 1, 0) <= 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR))) return;
        char bytes[16384]; const auto n = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (n < 0) { if (errno != EINTR && errno != EAGAIN) failInput("Could not read stdin."); return; }
        if (n == 0) { m_eof = true; if (!m_input.trimmed().isEmpty()) acceptLine(m_input); m_input.clear(); return; }
        m_input.append(bytes, n);
        for (;;) {
            const auto newline = m_input.indexOf('\n'); if (newline < 0) break;
            const auto line = m_input.left(newline); m_input.remove(0, newline + 1);
            if (!line.trimmed().isEmpty()) acceptLine(line);
            if (m_exitCode == 2) { m_input.clear(); break; }
        }
        if (m_input.size() > maxInput) failInput("A request exceeds the 1 MiB input limit.");
    }

    void acceptLine(const QByteArray &line)
    {
        if (line.size() > maxInput) { failInput("A request exceeds the 1 MiB input limit."); return; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            failInput("Expected one JSON object per line: " + error.errorString()); return;
        }
        enqueue(document.object());
    }

    void complete()
    {
        const auto op = m_request.value("op").toString();
        const bool collection = aliases().value(op, op) == "collectProject" && m_response.value("result").toBool();
        const int batch = aliases().value(op, op) == "queueFileBatch" ? m_response.value("result").toInt(-1) : -1;
        if (m_cancelled && m_response.value("ok").toBool()) m_response = failure("cancelled", "The operation was cancelled.");
        if (!m_errors.isEmpty() && m_response.value("ok").toBool()) m_response = failure("operation_failed", m_errors.join('\n'));
        if (batch >= 0 || !m_batchJobs.isEmpty()) {
            QJsonArray files; int failed = 0, cancelled = 0, paused = 0, pending = 0;
            for (const auto& value : m_backend.fileBatchJobs()) {
                const auto job = value.toMap();
                if (batch >= 0 ? job.value("batch").toInt() != batch : !m_batchJobs.contains(job.value("id").toInt())) continue;
                files.push_back(QJsonObject::fromVariantMap(job));
                const auto state = job.value("state").toString();
                failed += state == "failed"; cancelled += state == "cancelled"; paused += state == "paused";
                pending += state == "preparing" || state == "queued" || state == "reading" || state == "processing";
            }
            if (!pending && (failed || cancelled || paused) && m_response.value("ok").toBool())
                m_response = failure("batch_incomplete", QString("%1 files failed, %2 were cancelled and %3 are paused. Inspect files for each result.").arg(failed).arg(cancelled).arg(paused));
            if (batch >= 0) m_response["batch"] = batch;
            m_response["files"] = files;
        }
        if (collection)
            m_response["collection"] = QJsonObject::fromVariantMap(m_backend.collectionStatus());
        m_response["id"] = m_request.value("id"); m_response["type"] = "response";
        m_response["state"] = state();
        if (!m_warnings.isEmpty()) m_response["warnings"] = QJsonArray::fromStringList(m_warnings);
        if (!m_response.value("ok").toBool()) {
            if (!m_exitCode) m_exitCode = m_response.value("error").toObject().value("code") == "timeout" ? 124 : 1;
            if (!m_stream || m_initialLoad) { m_queue.clear(); m_savePath.clear(); m_eof = true; m_stream = false; }
        }
        remember(m_response);
        send(m_response); m_active = false; m_dispatched = false; m_initialLoad = false;
    }

    void finish()
    {
        if (m_finishing) return;
        if (m_backend.recording()) { m_backend.stop(); return; }
        if (!m_savePath.isEmpty() && m_exitCode == 0) {
            const auto path = std::exchange(m_savePath, {});
            enqueue({{"op", "saveAs"}, {"args", QJsonObject{{"path", path}}}, {"id", "save"}}); return;
        }
        m_finishing = true;
        m_backend.stop();
        if (m_backend.dirty() && !m_discard) {
            QString index;
            if (m_backend.prepareSessionCheckpoint(index)) {
                send({{"type", "session"}, {"ok", false}, {"error", QJsonObject{{"code", "unsaved_changes"},
                    {"message", "Unsaved changes were preserved in recovery. Save explicitly or use --save on the next run."}}},
                    {"recovery", index}, {"state", state()}});
                if (!m_exitCode) m_exitCode = 3;
            } else { send(failure("recovery_failed", m_backend.lastError())); if (!m_exitCode) m_exitCode = 1; }
        }
        QCoreApplication::exit(m_exitCode);
    }

    void tick()
    {
        readInput();
        if (interrupted) {
            m_exitCode = 128 + interrupted; interrupted = 0; m_eof = true; m_stream = false;
            m_queue.clear(); m_savePath.clear(); m_backend.cancelCommandWork();
            if (m_active) { m_response = failure("interrupted", "The operation was interrupted; cancellation was requested."); m_dispatched = true; }
        }
        if (!m_active && !m_queue.isEmpty()) {
            m_request = m_queue.dequeue(); m_errors.clear(); m_warnings.clear(); m_batchJobs.clear(); m_backend.clearError();
            m_active = true; m_dispatched = false; m_cancelled = false; m_waitForPlay = false; m_waitForStop = false; m_waitMs = 0; m_waitUntil.clear(); m_clock.start(); m_lastEvent = 0;
            m_requestTimeout = m_timeout;
            if (m_request.contains("timeoutMs")) {
                QVariant timeout;
                if (!convert(m_request.value("timeoutMs"), QMetaType::fromType<int>(), timeout) || timeout.toInt() < 1) {
                    m_response = failure("invalid_request", "timeoutMs must be a positive integer."); complete(); return;
                }
                m_requestTimeout = timeout.toInt();
            }
        }
        if (m_active && m_stream && !m_queue.isEmpty()) {
            const auto next = m_queue.head().value("op").toString();
            // Requests read together before the first tick must remain observable
            // while it runs, just like requests arriving after dispatch.
            if (cancellation(next) || (m_dispatched && (observation(next) || immediateStop(m_queue.head()))))
                immediate(m_queue.dequeue());
        }
        if (m_active) {
            const auto op = m_request.value("op").toString();
            const bool observe = observation(op);
            if (!m_dispatched && !observe && m_clock.elapsed() >= m_requestTimeout) {
                m_backend.cancelCommandWork();
                m_response = failure("timeout", "Deadline exceeded before the command could start; pending work was asked to cancel.");
                complete(); return;
            }
            if (!m_dispatched && m_cancelled && !observe) {
                // Cancelled while still waiting its turn: it is not run. Running
                // it and then answering "cancelled" told the caller an edit had
                // not happened when it had, and a retry applied it twice.
                m_response = failure("cancelled", "The operation was cancelled before it started.");
                complete(); return;
            }
            if (!m_dispatched && !observe && !m_errors.isEmpty()) {
                m_response = failure("operation_failed", m_errors.join('\n')); complete(); return;
            }
            // State-bearing saves wait for recording to finish. Stop must be
            // dispatchable while that save is queued, then obey its usual wait.
            if (!m_dispatched && (!m_backend.commandBusy() || observe || stopping(op))) {
                m_response = dispatch(m_request); m_dispatched = true;
                m_backend.synchronizeCommandState();
            }
            if (m_dispatched && (m_waitForPlay || m_waitForStop)) m_backend.synchronizeCommandState();
            const bool transportReady = (!m_waitForPlay || m_backend.commandPlaybackEpoch() != m_playEpoch)
                && (!m_waitForStop || (!m_backend.playing() && !m_backend.commandTransportPending()));
            const bool waited = m_cancelled || (transportReady && m_clock.elapsed() >= m_waitMs
                && (m_waitUntil != "stopped" || (!m_backend.playing() && !m_backend.recording()))
                && (m_waitUntil != "recording" || m_backend.recording()));
            if (!observe && m_clock.elapsed() >= m_requestTimeout) {
                m_backend.cancelCommandWork();
                m_response = failure("timeout", "The operation exceeded its deadline. Cancellation was requested; inspect before retrying because changes may have completed.");
                complete();
            } else if (m_dispatched && (!m_response.value("ok").toBool() || !m_request.value("wait").toBool(true)
                    || observe || (waited && !m_backend.commandBusy() && ((op != "stopRecording" && op != "record.stop" && op != "stop" && op != "transport.stop") || !m_backend.recording())))) {
                complete();
            } else if (m_events && m_clock.elapsed() - m_lastEvent >= 250) {
                m_lastEvent = m_clock.elapsed();
                send({{"type", "progress"}, {"id", m_request.value("id")}, {"elapsedMs", m_clock.elapsed()},
                    {"state", state()}, {"renderProgress", m_backend.renderProgress()},
                    {"spectralProgress", m_backend.spectralProgress()}, {"trackLoudnessProgress", m_backend.trackLoudnessProgress()}});
            }
        }
        if (m_eof && m_queue.isEmpty() && m_backend.recording()) {
            const auto name = aliases().value(m_request.value("op").toString(), m_request.value("op").toString());
            // Finish explicit waits/edits in order, but do not let a last Save
            // wait forever for the recording that EOF itself must finish.
            if (!m_active || name == "save" || name == "saveAs" || name == "autosaveNow")
                m_backend.stop();
        }
        if (m_eof && !m_active && m_queue.isEmpty() && !m_backend.commandBusy()) finish();
    }

    Backend &m_backend;
    int m_output, m_timeout;
    bool m_audio, m_events, m_discard;
    QString m_savePath, m_waitUntil;
    int m_waitMs = 0;
    quint32 m_playEpoch = 0;
    bool m_waitForPlay = false, m_waitForStop = false;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QQueue<QJsonObject> m_queue;
    QJsonObject m_request, m_response, m_results;
    QSet<int> m_batchJobs;
    qsizetype m_resultBytes = 0;
    QByteArray m_input;
    QStringList m_errors, m_warnings;
    qint64 m_lastEvent = 0;
    int m_requestTimeout = 0, m_exitCode = 0;
    bool m_initialLoad = false, m_cancelled = false, m_stream = false, m_eof = false, m_active = false, m_dispatched = false, m_finishing = false;
};
}

bool requested(const QStringList &arguments)
{
    return arguments.size() > 1 && (arguments[1] == "cli" || arguments[1] == "--cli");
}

int run(int &argc, char **argv, const QStringList &arguments)
{
    // Preserve a private protocol fd, and send library/plugin stdout diagnostics to
    // stderr. JSON responses must remain parseable even with chatty audio plugins.
    const int output = ::dup(STDOUT_FILENO);
    if (output < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 1;
    ::fcntl(output, F_SETFD, FD_CLOEXEC);
    const auto closeOutput = qScopeGuard([output] { ::close(output); });
    const auto emitJson = [output](QJsonObject value) {
        const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        QFile file; if (file.open(output, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) { file.write(bytes); file.flush(); }
    };
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    qputenv("QT_QPA_OFFSCREEN_NO_GLX", "1");
    // Pass no user options to Qt: the CLI parser owns all arguments, including -h.
    int guiArgc = 1;
    QGuiApplication app(guiArgc, argv);
    Q_UNUSED(argc);
    app.setOrganizationName("omaaudio"); app.setApplicationName("omaaudio"); app.setApplicationVersion(cli::version);
    QCommandLineParser parser;
    parser.setApplicationDescription("OmaAudio agent CLI (JSON protocol v1). No window or audio device is opened by default.\n"
        "Examples:\n  omaaudio cli schema\n  omaaudio cli --project session.omaaudio inspect\n"
        "  omaaudio cli --project session.omaaudio --save edited.omaaudio split '{\"frame\":48000}'\n"
        "  omaaudio cli run recipe.json\n  omaaudio cli --audio --stdio\n\n"
        "All Backend operations are discoverable with schema. Arguments are JSON objects (named) or arrays (positional).\n"
        "get/set access properties; inspect reads the complete session. run executes a JSON array of requests.\n"
        "Stdio accepts one {\"id\":1,\"op\":\"name\",\"args\":{...}} per line in one persistent session.\n"
        "Commands wait for background work by default; wait:false returns after acceptance.\n"
        "Unsaved work is preserved in recovery on exit (status 3); --discard explicitly allows dropping it.\n"
        "Exit codes: 0 success, 1 operation failure, 2 invalid input, 3 unsaved work, 124 deadline, 128+signal interruption.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"project", "Load a project bundle before executing commands.", "path"});
    parser.addOption({"save", "Save to this project path after every requested command succeeds.", "path"});
    parser.addOption({"audio", "Enable PipeWire playback, recording and monitoring."});
    parser.addOption({"stdio", "Keep one session, reading JSON requests from stdin until EOF or quit."});
    parser.addOption({"events", "Emit JSON progress events while waiting for operations."});
    parser.addOption({"discard", "Allow unsaved changes to be discarded when this session ends."});
    parser.addOption({"timeout", "Per-command deadline in milliseconds (positive integer).", "ms", "300000"});
    parser.addPositionalArgument("operation", "schema, inspect, get, set, run FILE, or any operation from schema.", "[operation]");
    parser.addPositionalArgument("arguments", "One JSON object/array; run takes a JSON recipe filename (- for stdin).", "[arguments]");
    auto args = arguments; args.removeAt(1); args[0] += " cli";
    if (!parser.parse(args)) { emitJson(failure("invalid_options", parser.errorText())); return 2; }
    if (parser.isSet("help") || parser.isSet("version")) {
        QFile file; if (!file.open(output, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) return 1;
        file.write((parser.isSet("help") ? parser.helpText() : QString("OmaAudio ") + cli::version + '\n').toUtf8()); file.flush(); return 0;
    }
    bool validTimeout = false; const int timeout = parser.value("timeout").toInt(&validTimeout);
    const auto positional = parser.positionalArguments();
    if (!validTimeout || timeout < 1 || positional.size() > 2 || (parser.isSet("stdio") && !positional.isEmpty())) {
        emitJson(failure("invalid_options", "Use a positive --timeout, one operation and optional JSON arguments, or --stdio.")); return 2;
    }
    QList<QJsonObject> requests;
    if (!parser.isSet("stdio")) {
        const QString op = positional.value(0, "schema");
        if (op == "run") {
            if (positional.size() != 2) { emitJson(failure("invalid_options", "run needs a JSON recipe filename, or - for stdin.")); return 2; }
            QFile file(positional[1]);
            const bool opened = positional[1] == "-" ? file.open(stdin, QIODevice::ReadOnly) : file.open(QIODevice::ReadOnly);
            if (!opened) { emitJson(failure("input_failed", file.errorString())); return 2; }
            const auto bytes = file.read(maxInput + 1);
            QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
            if (bytes.size() > maxInput || error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > maxRequests) {
                emitJson(failure("invalid_recipe", "A recipe must be a JSON array, at most 1 MiB and 10000 requests.")); return 2;
            }
            for (const auto &value : document.array()) {
                if (!value.isObject()) { emitJson(failure("invalid_recipe", "Every recipe entry must be a request object.")); return 2; }
                requests.append(value.toObject());
            }
        } else {
            QJsonValue parameters = QJsonObject{};
            if (positional.size() == 2) {
                QJsonParseError error; const auto document = QJsonDocument::fromJson(positional[1].toUtf8(), &error);
                if (error.error != QJsonParseError::NoError || document.isNull()) { emitJson(failure("invalid_arguments", error.errorString())); return 2; }
                parameters = document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
            }
            requests.append({{"op", op}, {"args", parameters}, {"id", 1}});
        }
    }
    ::signal(SIGPIPE, SIG_IGN);
    const auto oldInt = ::signal(SIGINT, interrupt), oldTerm = ::signal(SIGTERM, interrupt);
    const auto restoreSignals = qScopeGuard([=] { ::signal(SIGINT, oldInt); ::signal(SIGTERM, oldTerm); });
    pw_init(nullptr, nullptr); const auto cleanup = qScopeGuard([] { pw_deinit(); });
    Backend backend;
    backend.setAutosaveSeconds(0);
    if (parser.isSet("audio") && !backend.start()) { emitJson(failure("audio_unavailable", backend.lastError())); return 1; }
    Session session(backend, output, timeout, parser.isSet("audio"), parser.isSet("events"), parser.isSet("discard"), parser.value("save"));
    if (parser.isSet("project")) session.initialLoad(parser.value("project"));
    for (const auto &request : requests) session.enqueue(request);
    session.input(parser.isSet("stdio"));
    if (!parser.isSet("stdio")) session.endInput();
    app.exec();
    return session.exitCode();
}
}
