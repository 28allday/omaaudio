#include "filebatch_store.h"
#include <QDateTime>
#include "io/plugin_state.h"
#include "io/project_storage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace omawave;
namespace {
constexpr qint64 manifestLimit = 8 * 1024 * 1024;
constexpr std::size_t jobLimit = 512, insertLimit = 128, controlLimit = 4096, totalControls = 65536;
void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}
bool pathValid(const QString& value)
{
    return !value.isEmpty() && value.size() <= 16384 && QDir::isAbsolutePath(value)
        && !value.contains(QChar::Null);
}
QString string(const QJsonObject& object, const char* name, qsizetype maximum = 16384, bool empty = true)
{
    const auto node = object.value(QLatin1String(name));
    require(node.isString(), "queue text has the wrong type");
    const auto value = node.toString();
    require(value.size() <= maximum && !value.contains(QChar::Null) && (empty || !value.isEmpty()),
        "queue text exceeds its supported range");
    return value;
}
bool boolean(const QJsonObject& object, const char* name)
{
    const auto value = object.value(QLatin1String(name));
    require(value.isBool(), "queue boolean has the wrong type");
    return value.toBool();
}
double number(const QJsonObject& object, const char* name, double minimum, double maximum)
{
    const auto value = object.value(QLatin1String(name));
    require(value.isDouble(), "queue number has the wrong type");
    const auto n = value.toDouble();
    require(std::isfinite(n) && n >= minimum && n <= maximum, "queue number is out of range");
    return n;
}
qint64 integer(const QJsonObject& object, const char* name, qint64 minimum, qint64 maximum)
{
    const auto n = number(object, name, double(minimum), double(maximum));
    require(std::floor(n) == n, "queue integer is fractional");
    return qint64(n);
}
QJsonArray array(const QJsonObject& object, const char* name, std::size_t maximum)
{
    const auto value = object.value(QLatin1String(name));
    require(value.isArray(), "queue list has the wrong type");
    auto result = value.toArray();
    require(std::size_t(result.size()) <= maximum, "queue list exceeds its limit");
    return result;
}
QJsonObject object(const QJsonValue& value)
{
    require(value.isObject(), "queue record has the wrong type");
    return value.toObject();
}
bool terminal(const QString& state) { return state == "done" || state == "failed" || state == "cancelled"; }
bool validState(const QString& state)
{
    return terminal(state) || state == "paused" || state == "preparing" || state == "queued"
        || state == "reading" || state == "processing";
}
bool same(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
void boundedJson(const QJsonValue& value, unsigned depth, std::size_t& nodes)
{
    require(depth <= 8 && ++nodes <= totalControls * 8, "queue display metadata exceeds its limit");
    if (value.isString())
        require(value.toString().size() <= 16384 && !value.toString().contains(QChar::Null),
            "queue display text exceeds its limit");
    else if (value.isDouble())
        require(std::isfinite(value.toDouble()), "invalid queue display number");
    else if (value.isArray())
        for (const auto& child : value.toArray())
            boundedJson(child, depth + 1, nodes);
    else if (value.isObject()) {
        const auto fields = value.toObject();
        for (auto i = fields.begin(); i != fields.end(); ++i) {
            require(i.key().size() <= 128, "queue display key exceeds its limit");
            boundedJson(i.value(), depth + 1, nodes);
        }
    } else
        require(value.isBool() || value.isNull(), "invalid queue display metadata");
}
QVariantMap display(const QJsonValue& value)
{
    std::size_t nodes = 0;
    boundedJson(value, 0, nodes);
    return object(value).toVariantMap();
}
void validateReport(const QJsonObject& report)
{
    if (report.isEmpty())
        return;
    constexpr qint64 maximumFrames = 9007199254740991LL;
    (void)integer(report, "frames", 0, maximumFrames);
    (void)integer(report, "sampleRate", 1, INT_MAX);
    (void)integer(report, "channels", 1, 256);
    const auto container = string(report, "container", 32, false);
    require(render::containerFromId(container.toStdString()).has_value(), "invalid batch report format");
    (void)number(
        report, "appliedGainDb", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    (void)boolean(report, "ceilingLimited");
    (void)boolean(report, "metadataWritten");
    (void)integer(report, "markersWritten", 0, INT_MAX);
    for (const auto* section : { "measured", "delivered" }) {
        const auto fields = object(report.value(QLatin1String(section)));
        (void)integer(fields, "frames", 0, maximumFrames);
        for (const auto* key : { "integrated", "range", "truePeak", "momentaryMax", "shortTermMax" }) {
            const auto value = fields.value(QLatin1String(key));
            if (value.isString() && value.toString() == "-inf")
                continue;
            (void)number(fields, key, -std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        }
    }
}
QJsonObject reportJson(const QVariantMap& report)
{
    auto result = QJsonObject::fromVariantMap(report);
    for (const auto* section : { "measured", "delivered" }) {
        if (!report.contains(QLatin1String(section)))
            continue;
        const auto original = report.value(QLatin1String(section)).toMap();
        auto fields = result.value(QLatin1String(section)).toObject();
        for (const auto* key : { "integrated", "range", "truePeak", "momentaryMax", "shortTermMax" }) {
            if (!original.contains(QLatin1String(key)))
                continue;
            const auto value = original.value(QLatin1String(key)).toDouble();
            if (std::isinf(value) && value < 0)
                fields[QLatin1String(key)] = QStringLiteral("-inf");
        }
        result[QLatin1String(section)] = fields;
    }
    (void)display(result);
    validateReport(result);
    return result;
}
QVariantMap reportFrom(const QJsonValue& value)
{
    validateReport(object(value));
    auto result = display(value);
    for (const auto* section : { "measured", "delivered" }) {
        if (!result.contains(QLatin1String(section)))
            continue;
        auto fields = result.value(QLatin1String(section)).toMap();
        for (const auto* key : { "integrated", "range", "truePeak", "momentaryMax", "shortTermMax" })
            if (fields.value(QLatin1String(key)).metaType().id() == QMetaType::QString
                && fields.value(QLatin1String(key)).toString() == "-inf")
                fields[QLatin1String(key)] = -std::numeric_limits<double>::infinity();
        result[QLatin1String(section)] = fields;
    }
    return result;
}

void validControl(const session::Insert& insert, const session::InsertControl& control)
{
    const bool vst = insert.uri.starts_with("vst3:"), clap = insert.uri.starts_with("clap:");
    require(std::isfinite(control.value)
            && (vst ? control.value >= 0 && control.value <= 1
                    : std::abs(control.value) <= std::numeric_limits<float>::max()),
        "invalid captured batch control value");
    require(control.port <= (vst ? 0x7fffffffu
                    : clap       ? 0xfffffffeu
                                 : std::numeric_limits<std::uint32_t>::max()),
        "invalid captured batch control identity");
    require(!control.automation.hasData(), "batch controls must not contain timeline automation");
    for (const auto& actual : control.instanceAutomation)
        if (actual) {
            require(!actual->curve.hasData() && std::isfinite(actual->base)
                    && (vst ? actual->base >= 0 && actual->base <= 1
                            : std::abs(actual->base) <= std::numeric_limits<float>::max()),
                "invalid static per-instance batch control");
        }
    require(!control.fromState || !insert.instanceState.empty(),
        "state-authoritative batch controls need captured native state");
}
QJsonObject exportJson(const render::ExportSpec& spec)
{
    const auto container = render::containerId(spec.container);
    require(!container.empty(), "invalid batch output format");
    const auto bits = render::bitsOf(spec.sampleFormat);
    require(render::sampleFormatFromBits(bits).has_value(), "invalid batch sample format");
    require(spec.path.empty() && !spec.replaceExisting,
        "batch output must use its job path and refuse replacement");
    require(
        (!spec.sampleRate || (spec.sampleRate >= 8000 && spec.sampleRate <= 192000)) && spec.channels <= 2,
        "invalid batch output rate or layout");
    require(spec.bitrateKbps == 0 || (spec.bitrateKbps >= 64 && spec.bitrateKbps <= 320),
        "invalid batch bitrate");
    require(std::isfinite(spec.targetLufs) && spec.targetLufs >= -70 && spec.targetLufs <= 0
            && std::isfinite(spec.ceilingDbTp) && spec.ceilingDbTp >= -24 && spec.ceilingDbTp <= 0
            && std::isfinite(spec.peakTargetDb) && spec.peakTargetDb >= -90 && spec.peakTargetDb <= 0,
        "invalid batch level settings");
    QString normalise;
    switch (spec.normalise) {
    case render::Normalise::None:
        normalise = "none";
        break;
    case render::Normalise::Peak:
        normalise = "peak";
        break;
    case render::Normalise::Loudness:
        normalise = "loudness";
        break;
    default:
        throw std::runtime_error("invalid batch normalisation");
    }
    return { { "format", QString::fromStdString(std::string(container)) }, { "bitDepth", bits },
        { "sampleRate", qint64(spec.sampleRate) }, { "channels", int(spec.channels) },
        { "dither", spec.dither }, { "bitrateKbps", int(spec.bitrateKbps) }, { "normalise", normalise },
        { "targetLufs", double(spec.targetLufs) }, { "ceilingDbTp", double(spec.ceilingDbTp) },
        { "peakTargetDb", double(spec.peakTargetDb) }, { "writeMetadata", spec.writeMetadata },
        { "writeMarkers", spec.writeMarkers } };
}
void refreshDisplay(FileBatchProcessor::Options& options)
{
    const auto output = exportJson(options.output);
    for (const auto* key :
        { "format", "bitDepth", "sampleRate", "channels", "dither", "bitrateKbps", "normalise" })
        options.settings[QLatin1String(key)] = output.value(QLatin1String(key)).toVariant();
    for (const auto* key : { "targetLufs", "ceilingDbTp", "peakTargetDb" }) {
        const auto actual = output.value(QLatin1String(key)).toDouble();
        const auto shown = QJsonValue::fromVariant(options.settings.value(QLatin1String(key)));
        if (!shown.isDouble() || !std::isfinite(shown.toDouble())
            || std::abs(shown.toDouble()) > std::numeric_limits<float>::max()
            || float(shown.toDouble()) != float(actual))
            options.settings[QLatin1String(key)] = actual;
    }
    options.settings["includeMetadata"] = options.output.writeMetadata && options.output.writeMarkers;
    options.settings["effects"] = options.effects;
    QVariantList inserts;
    for (const auto& insert : options.inserts) {
        QVariantList controls;
        for (const auto& control : insert.controls) {
            QVariantList bases;
            for (const auto& actual : control.instanceAutomation)
                bases.push_back(actual ? QVariant(actual->base) : QVariant::fromValue(nullptr));
            controls.push_back(QVariantMap {
                { "port", control.port }, { "value", control.value }, { "instanceBases", bases } });
        }
        inserts.push_back(QVariantMap { { "uri", QString::fromStdString(insert.uri) },
            { "controls", controls }, { "legacyClapControls", insert.legacyClapControls } });
    }
    options.settings["inserts"] = inserts;
}
bool outputMatches(
    const QString& input, const QString& output, const FileBatchProcessor::Options& options, bool resumable)
{
    if (!pathValid(output) || QFileInfo(output).absolutePath() != options.directory)
        return false;
    if (!resumable)
        return true;
    const auto extension = render::containerExtension(options.output.container);
    const auto name = QFileInfo(input).completeBaseName() + options.suffix + "."
        + QString::fromUtf8(extension.data(), qsizetype(extension.size()));
    return output == QDir(options.directory).filePath(name) && name.toUtf8().size() <= 255;
}

render::ExportSpec exportFrom(const QJsonObject& value)
{
    render::ExportSpec result;
    result.replaceExisting = false;
    const auto container = render::containerFromId(string(value, "format", 16, false).toStdString());
    require(container.has_value(), "unsupported stored batch format");
    result.container = *container;
    const auto format = render::sampleFormatFromBits(int(integer(value, "bitDepth", 16, 32)));
    require(format.has_value(), "unsupported stored sample format");
    result.sampleFormat = *format;
    result.sampleRate = std::uint32_t(integer(value, "sampleRate", 0, 192000));
    result.channels = std::uint32_t(integer(value, "channels", 0, 2));
    result.dither = boolean(value, "dither");
    result.bitrateKbps = unsigned(integer(value, "bitrateKbps", 0, 320));
    const auto mode = string(value, "normalise", 16, false);
    require(mode == "none" || mode == "peak" || mode == "loudness", "invalid stored normalisation");
    result.normalise = mode == "none" ? render::Normalise::None
        : mode == "peak"              ? render::Normalise::Peak
                                      : render::Normalise::Loudness;
    result.targetLufs = float(number(value, "targetLufs", -70, 0));
    result.ceilingDbTp = float(number(value, "ceilingDbTp", -24, 0));
    result.peakTargetDb = float(number(value, "peakTargetDb", -90, 0));
    result.writeMetadata = boolean(value, "writeMetadata");
    result.writeMarkers = boolean(value, "writeMarkers");
    (void)exportJson(result);
    return result;
}
}

struct FileBatchStore::Impl {
    QString root, error;
    std::unique_ptr<QLockFile> lock;
    int directory = -1;
    bool available = true, existed = false;
    struct stat manifest { };
    std::map<std::string, struct stat> published;
    explicit Impl(QString value)
        : root(std::move(value))
    {
    }
    ~Impl()
    {
        if (directory >= 0)
            ::close(directory);
    }
    std::filesystem::path pinned() const { return "/proc/self/fd/" + std::to_string(directory); }
    void verifyRoot() const
    {
        struct stat pinned { }, named { };
        require(::fstat(directory, &pinned) == 0 && ::lstat(QFile::encodeName(root).constData(), &named) == 0
                && S_ISDIR(named.st_mode) && pinned.st_dev == named.st_dev && pinned.st_ino == named.st_ino,
            "batch queue directory changed during storage");
    }
    void unchangedManifest() const
    {
        struct stat current { };
        const auto found = ::fstatat(directory, "queue.json", &current, AT_SYMLINK_NOFOLLOW);
        require(existed ? found == 0 && S_ISREG(current.st_mode) && same(manifest, current)
                        : found < 0 && errno == ENOENT,
            "batch queue manifest changed outside its owner; reopen the queue before continuing");
    }
    // Crash debris: temporaries and sidecars written before a manifest that never
    // published. This process holds the lifetime lock, so nothing else is writing;
    // anything here that the manifest does not reference cannot be used again.
    std::size_t housekeep(const std::set<std::string>& referenced) const
    {
        constexpr std::size_t limit = 512; // bounded per open; the rest go next time
        std::size_t removed = 0;
        const auto sweep = [&](int at, const QString& path, const auto& debris) {
            QDir listing(path);
            for (const auto& name : listing.entryList(QDir::Files | QDir::Hidden | QDir::NoSymLinks)) {
                if (removed >= limit) return;
                struct stat entry { };
                const auto encoded = QFile::encodeName(name);
                if (!debris(name) || ::fstatat(at, encoded.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0
                    || !S_ISREG(entry.st_mode)) continue;
                if (::unlinkat(at, encoded.constData(), 0) == 0) ++removed;
            }
        };
        sweep(directory, QStringLiteral("/proc/self/fd/%1").arg(directory),
              [](const QString& name) { return name.startsWith(QLatin1String("queue.json.tmp-")); });
        const int state = ::openat(directory, "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (state >= 0) {
            // An unreferenced sidecar is removed only when its header proves this
            // application wrote it; anything else may belong to someone else.
            const auto ours = [state](const QString& name) {
                const int fd = ::openat(state, QFile::encodeName(name).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
                if (fd < 0) return false;
                unsigned char header[12]{};
                const auto count = ::read(fd, header, sizeof header);
                ::close(fd);
                static constexpr unsigned char magic[8]{'O','M','A','P','S','T','A','T'};
                return count == sizeof header && std::equal(magic, magic + 8, header)
                    && header[8] == 1 && header[9] == 0 && header[10] == 0 && header[11] == 0;
            };
            sweep(state, QStringLiteral("/proc/self/fd/%1").arg(state), [&](const QString& name) {
                if (name.startsWith(QLatin1String(".tmp-"))) return true;
                if (!name.endsWith(QLatin1String(".omas"))) return false;
                const auto id = name.chopped(5).toStdString();
                const auto uuid = session::Uuid::parse(id);
                return uuid && uuid->toString() == id && !referenced.contains(id) && ours(name);
            });
            ::fsync(state);
            ::close(state);
        }
        return removed;
    }
    void fail(const std::exception& exception)
    {
        available = false;
        error = QStringLiteral("Batch queue storage is unavailable: %1")
                    .arg(QString::fromUtf8(exception.what()));
    }
};
FileBatchStore::FileBatchStore(QString root)
    : m(std::make_unique<Impl>(std::move(root)))
{
    if (m->root.isEmpty())
        return;
    try {
        require(pathValid(m->root), "choose an absolute bounded batch queue directory");
        require(QDir().mkpath(m->root), "could not create batch queue directory");
        m->directory
            = ::open(QFile::encodeName(m->root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        require(m->directory >= 0, "batch queue directory is unavailable or symbolic");
        m->verifyRoot();
        m->lock = std::make_unique<QLockFile>(m->root + "/queue.lock");
        m->lock->setStaleLockTime(0);
        require(m->lock->tryLock(0), "another OmaAudio instance owns this batch queue");
        m->verifyRoot();
    } catch (const std::exception& error) {
        m->fail(error);
    }
}
FileBatchStore::~FileBatchStore() = default;

bool FileBatchStore::reset(QString& error)
{
    error.clear();
    if (m->root.isEmpty()) { m->available = true; m->error.clear(); return true; }
    if (!m->lock || !m->lock->isLocked() || m->directory < 0) {
        error = m->error.isEmpty() ? QStringLiteral("The batch queue cannot be reset here.")
                                   : m->error + QStringLiteral(" Close the other OmaAudio first, then reset.");
        return false;
    }
    try {
        m->verifyRoot();
        // Keep the damaged manifest beside the queue for diagnosis; only the three
        // newest are retained. Outputs already written are never touched.
        struct stat current { };
        if (::fstatat(m->directory, "queue.json", &current, AT_SYMLINK_NOFOLLOW) == 0) {
            const auto aside = QStringLiteral("queue.damaged-%1.json")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")));
            require(::renameat(m->directory, "queue.json", m->directory, QFile::encodeName(aside).constData()) == 0,
                "could not set the damaged batch queue aside");
        } else require(errno == ENOENT, "could not inspect the batch queue manifest");
        QDir listing(QStringLiteral("/proc/self/fd/%1").arg(m->directory));
        auto damaged = listing.entryList({ QStringLiteral("queue.damaged-*.json") }, QDir::Files | QDir::NoSymLinks, QDir::Name);
        while (damaged.size() > 3) {
            (void)::unlinkat(m->directory, QFile::encodeName(damaged.takeFirst()).constData(), 0);
        }
        (void)m->housekeep({});
        ::fsync(m->directory);
        m->available = true;
        m->error.clear();
        m->existed = false;
        m->published.clear();
        return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("Could not reset the batch queue: %1").arg(QString::fromUtf8(exception.what()));
        return false;
    }
}
QVariantMap FileBatchStore::status() const
{
    return { { "available", m->available }, { "error", m->error },
        { "path", m->root.isEmpty() ? QString { } : m->root + "/queue.json" } };
}

bool FileBatchStore::load(std::vector<FileBatchProcessor::Job>& jobs, int& nextJob, int& nextBatch)
{
    if (!m->available)
        return false;
    if (m->root.isEmpty())
        return true;
    try {
        m->verifyRoot();
        const int fd = ::openat(m->directory, "queue.json", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            require(errno == ENOENT, "could not open batch queue manifest");
            (void)m->housekeep({});
            return true;
        }
        struct Close {
            int fd;
            ~Close() { ::close(fd); }
        } close { fd };
        struct stat before { }, after { };
        require(::fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0
                && before.st_size <= manifestLimit,
            "batch queue manifest is not a bounded regular file");
        QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
        qsizetype at = 0;
        while (at < bytes.size()) {
            const auto count = ::read(fd, bytes.data() + at, std::size_t(bytes.size() - at));
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "could not read complete batch queue");
            at += count;
        }
        require(::fstat(fd, &after) == 0 && same(before, after), "batch queue changed during reading");
        m->manifest = after;
        m->existed = true;
        m->unchangedManifest();
        m->verifyRoot();
        QJsonParseError parsed;
        const auto document = QJsonDocument::fromJson(bytes, &parsed);
        require(parsed.error == QJsonParseError::NoError && document.isObject(), "corrupt batch queue JSON");
        const auto data = document.object();
        require(integer(data, "version", 1, 1) == 1, "unsupported batch queue version");
        const auto nextFile = int(integer(data, "nextJob", 1, INT_MAX)),
                   nextGroup = int(integer(data, "nextBatch", 1, INT_MAX));
        std::map<int, std::shared_ptr<const FileBatchProcessor::Options>> batches;
        session::PluginStateBudget budget;
        std::map<std::string, session::PluginState> states;
        std::size_t controls = 0;
        for (const auto& entry : array(data, "batches", jobLimit)) {
            const auto batch = object(entry);
            const auto id = int(integer(batch, "id", 1, INT_MAX - 1));
            require(id < nextGroup && !batches.contains(id), "duplicate or inconsistent batch identity");
            auto options = std::make_shared<FileBatchProcessor::Options>();
            options->directory = string(batch, "directory");
            require(pathValid(options->directory), "invalid batch output directory");
            options->suffix = string(batch, "suffix", 128);
            require(!options->suffix.contains('/') && !options->suffix.contains('\\'),
                "invalid batch output suffix");
            options->effects = string(batch, "effects", 4096);
            options->effectsCaptured = boolean(batch, "effectsCaptured");
            options->settings = display(batch.value("settings"));
            options->output = exportFrom(object(batch.value("output")));
            for (const auto& item : array(batch, "inserts", insertLimit)) {
                const auto stored = object(item);
                session::Insert insert;
                insert.uri = string(stored, "uri", 4096, false).toStdString();
                require(session::PluginState::validUri(insert.uri), "invalid stored effect URI");
                insert.bypass = boolean(stored, "bypass");
                insert.legacyClapControls = boolean(stored, "legacyClapControls");
                require(!insert.legacyClapControls || insert.uri.starts_with("clap:"),
                    "legacy control addressing needs a CLAP effect");
                for (const auto& ref : array(stored, "states", 2)) {
                    require(ref.isString(), "invalid native-state reference");
                    const auto identity = session::Uuid::parse(ref.toString().toStdString());
                    require(identity && identity->toString() == ref.toString().toStdString(),
                        "invalid native-state UUID");
                    const auto key = identity->toString();
                    if (!states.contains(key)) {
                        std::string problem;
                        auto state = io::readPluginState(m->pinned(), *identity, insert.uri,
                            session::PluginState::kProjectLimit - budget.bytes(), problem);
                        require(state.has_value(), problem.c_str());
                        states.emplace(key, *state);
                    }
                    require(states.at(key).uri() == insert.uri, "native state belongs to another effect");
                    std::string problem;
                    require(budget.add(states.at(key), problem), problem.c_str());
                    insert.instanceState.push_back(states.at(key));
                }
                QSet<quint32> ports;
                for (const auto& item : array(stored, "controls", controlLimit)) {
                    require(++controls <= totalControls, "too many stored batch controls");
                    const auto storedControl = object(item);
                    session::InsertControl control;
                    control.port = std::uint32_t(integer(storedControl, "port", 0, UINT32_MAX));
                    require(!ports.contains(control.port), "duplicate batch control identity");
                    ports.insert(control.port);
                    control.value = number(storedControl, "value", -std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::max());
                    control.fromState = boolean(storedControl, "fromState");
                    const auto actual = array(storedControl, "instanceBases", 2);
                    require(actual.size() == 2, "batch controls need two optional instance bases");
                    for (int i = 0; i < 2; ++i)
                        if (!actual[i].isNull()) {
                            require(actual[i].isDouble() && std::isfinite(actual[i].toDouble()),
                                "invalid per-instance control base");
                            control.instanceAutomation[i]
                                = session::InstanceAutomation { actual[i].toDouble(), { } };
                        }
                    validControl(insert, control);
                    insert.controls.push_back(std::move(control));
                }
                options->inserts.push_back(std::move(insert));
            }
            refreshDisplay(*options);
            batches.emplace(id, std::move(options));
        }
        std::vector<FileBatchProcessor::Job> restored;
        std::set<int> ids, usedBatches;
        QSet<QString> reservations;
        for (const auto& entry : array(data, "jobs", jobLimit)) {
            const auto row = object(entry);
            FileBatchProcessor::Job job;
            job.id = int(integer(row, "id", 1, INT_MAX - 1));
            job.batch = int(integer(row, "batch", 1, INT_MAX - 1));
            require(job.id < nextFile && ids.insert(job.id).second && batches.contains(job.batch),
                "invalid or duplicate batch job identity");
            usedBatches.insert(job.batch);
            job.options = batches.at(job.batch);
            job.input = string(row, "input");
            job.output = string(row, "output");
            job.state = string(row, "state", 16, false);
            require(validState(job.state), "invalid batch job state");
            require(outputMatches(job.input, job.output, *job.options, !terminal(job.state)),
                "stored output differs from its captured destination");
            if (!terminal(job.state)) {
                require(!reservations.contains(job.output), "duplicate reserved batch output");
                reservations.insert(job.output);
            }
            require(terminal(job.state)
                    || (pathValid(job.input) && pathValid(job.output) && job.input != job.output),
                "invalid resumable batch file paths");
            job.message = string(row, "message");
            job.warning = string(row, "warning");
            job.progress = float(number(row, "progress", 0, 1));
            job.cancelRequested = boolean(row, "cancelRequested");
            job.report = reportFrom(row.value("report"));
            if (!terminal(job.state)) {
                if (job.cancelRequested) {
                    job.state = "cancelled";
                    job.message = "Cancelled before the previous queue closed; check any existing output "
                                  "before retrying.";
                } else if (!job.options->effectsCaptured) {
                    job.state = "failed";
                    job.message
                        = "Effect capture did not finish. Queue this file again from the intended session.";
                } else {
                    job.state = "paused";
                    job.message = "Restored paused. Resume to process this file with its captured settings.";
                }
                job.progress = 0;
            }
            restored.push_back(std::move(job));
        }
        require(usedBatches.size() == batches.size(), "batch queue contains unreferenced options");
        for (const auto& [id, state] : states) {
            struct stat info { };
            const auto name = "state/" + id + ".omas";
            require(::fstatat(m->directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISREG(info.st_mode),
                "loaded native state disappeared");
            m->published[id] = info;
        }
        {
            std::set<std::string> referenced;
            for (const auto& [id, state] : states) referenced.insert(id);
            (void)m->housekeep(referenced);
        }
        jobs = std::move(restored);
        nextJob = nextFile;
        nextBatch = nextGroup;
        return true;
    } catch (const std::exception& error) {
        m->fail(error);
        return false;
    }
}

bool FileBatchStore::save(
    const std::vector<FileBatchProcessor::Job>& jobs, int nextJob, int nextBatch, QString& error)
{
    error.clear();
    if (!m->available) {
        error = m->error;
        return false;
    }
    bool writing = false;
    try {
        require(jobs.size() <= jobLimit && nextJob > 0 && nextBatch > 0,
            "batch queue exceeds its identity or row limit");
        std::map<int, std::shared_ptr<const FileBatchProcessor::Options>> batches;
        std::set<int> ids;
        QSet<QString> reservations;
        QJsonArray rows;
        for (const auto& job : jobs) {
            require(job.id > 0 && job.id < nextJob && job.batch > 0 && job.batch < nextBatch
                    && ids.insert(job.id).second && job.options,
                "invalid batch job identity");
            require(validState(job.state) && job.input.size() <= 16384 && job.output.size() <= 16384
                    && !job.input.contains(QChar::Null) && !job.output.contains(QChar::Null)
                    && (terminal(job.state)
                        || (pathValid(job.input) && pathValid(job.output) && job.input != job.output))
                    && std::isfinite(job.progress) && job.progress >= 0 && job.progress <= 1,
                "invalid batch job state or paths");
            require(outputMatches(job.input, job.output, *job.options, !terminal(job.state)),
                "batch output differs from its captured destination");
            if (!terminal(job.state)) {
                require(!reservations.contains(job.output), "duplicate reserved batch output");
                reservations.insert(job.output);
            }
            require(!batches.contains(job.batch) || batches.at(job.batch) == job.options,
                "a batch has inconsistent captured settings");
            batches[job.batch] = job.options;
            require(
                job.message.size() <= 16384 && job.warning.size() <= 16384, "batch status text is too long");
            const auto report = reportJson(job.report);
            rows.push_back(QJsonObject { { "id", job.id }, { "batch", job.batch }, { "input", job.input },
                { "output", job.output }, { "state", job.state }, { "message", job.message },
                { "warning", job.warning }, { "progress", double(job.progress) },
                { "cancelRequested", job.cancelRequested }, { "report", report } });
        }
        QJsonArray captured;
        session::PluginStateBudget budget;
        std::map<std::string, session::PluginState> states;
        std::size_t controls = 0;
        for (const auto& [id, pointer] : batches) {
            const auto& options = *pointer;
            require(pathValid(options.directory) && options.suffix.size() <= 128
                    && !options.suffix.contains('/') && !options.suffix.contains('\\')
                    && !options.suffix.contains(QChar::Null) && options.effects.size() <= 4096
                    && !options.effects.contains(QChar::Null) && options.inserts.size() <= insertLimit,
                "invalid batch options");
            QJsonArray inserts;
            for (const auto& insert : options.inserts) {
                require(session::PluginState::validUri(insert.uri)
                        && (!insert.legacyClapControls || insert.uri.starts_with("clap:"))
                        && insert.instanceState.size() <= 2 && insert.controls.size() <= controlLimit,
                    "invalid captured batch effect");
                QJsonArray native;
                for (const auto& state : insert.instanceState) {
                    std::string problem;
                    require(state.uri() == insert.uri && budget.add(state, problem),
                        problem.empty() ? "native state belongs to another effect" : problem.c_str());
                    states.emplace(state.id().toString(), state);
                    native.push_back(QString::fromStdString(state.id().toString()));
                }
                QJsonArray parameters;
                QSet<quint32> ports;
                for (const auto& control : insert.controls) {
                    require(++controls <= totalControls && !ports.contains(control.port),
                        "too many or duplicate captured batch controls");
                    ports.insert(control.port);
                    validControl(insert, control);
                    QJsonArray bases;
                    for (const auto& actual : control.instanceAutomation)
                        bases.push_back(actual ? QJsonValue(actual->base) : QJsonValue(QJsonValue::Null));
                    parameters.push_back(
                        QJsonObject { { "port", qint64(control.port) }, { "value", control.value },
                            { "fromState", control.fromState }, { "instanceBases", bases } });
                }
                inserts.push_back(QJsonObject { { "uri", QString::fromStdString(insert.uri) },
                    { "bypass", insert.bypass }, { "legacyClapControls", insert.legacyClapControls },
                    { "states", native }, { "controls", parameters } });
            }
            auto shown = options;
            refreshDisplay(shown);
            const auto settings = QJsonObject::fromVariantMap(shown.settings);
            (void)display(settings);
            captured.push_back(QJsonObject { { "id", id }, { "directory", options.directory },
                { "suffix", options.suffix }, { "effects", options.effects },
                { "effectsCaptured", options.effectsCaptured }, { "settings", settings },
                { "output", exportJson(options.output) }, { "inserts", inserts } });
        }
        const auto bytes = QJsonDocument(
            QJsonObject { { "version", 1 }, { "nextJob", nextJob }, { "nextBatch", nextBatch },
                { "batches", captured },
                { "jobs",
                    rows } }).toJson(QJsonDocument::Compact);
        require(bytes.size() <= manifestLimit, "batch queue metadata exceeds 8 MiB");
        if (m->root.isEmpty())
            return true;
        writing = true;
        m->verifyRoot();
        m->unchangedManifest();
        for (const auto& [id, state] : states) {
            const auto name = "state/" + id + ".omas";
            struct stat current { };
            const bool cached = m->published.contains(id)
                && ::fstatat(m->directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISREG(current.st_mode) && same(current, m->published.at(id));
            if (!cached) {
                const auto problem = io::writePluginState(m->pinned(), state);
                require(problem.empty(), problem.c_str());
                require(::fstatat(m->directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
                        && S_ISREG(current.st_mode),
                    "captured batch native state disappeared");
                m->published[id] = current;
            }
        }
        m->verifyRoot();
        m->unchangedManifest();
        const auto problem = io::storage::writeText(
            m->pinned() / "queue.json", std::string_view(bytes.constData(), std::size_t(bytes.size())));
        require(problem.empty(), problem.c_str());
        require(::fstatat(m->directory, "queue.json", &m->manifest, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISREG(m->manifest.st_mode),
            "saved batch queue disappeared");
        m->existed = true;
        m->verifyRoot();
        // State files are immutable and only this locked queue owns this directory.
        // Prune solely UUID-named regular sidecars after the new manifest is durable.
        const int sidecars = ::openat(m->directory, "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (sidecars >= 0) {
            struct Close {
                int fd;
                ~Close() { ::close(fd); }
            } close { sidecars };
            struct stat pinnedState { }, namedState { };
            const auto attached = [&] {
                return ::fstat(sidecars, &pinnedState) == 0
                    && ::fstatat(m->directory, "state", &namedState, AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISDIR(namedState.st_mode) && pinnedState.st_dev == namedState.st_dev
                    && pinnedState.st_ino == namedState.st_ino;
            };
            QDir stateDir(QStringLiteral("/proc/self/fd/%1").arg(sidecars));
            for (const auto& name : stateDir.entryList({ "*.omas" }, QDir::Files | QDir::NoSymLinks)) {
                const auto id = name.chopped(5).toStdString();
                const auto uuid = session::Uuid::parse(id);
                struct stat entry { };
                if (attached() && uuid && uuid->toString() == id && m->published.contains(id)
                    && !states.contains(id)
                    && ::fstatat(sidecars, QFile::encodeName(name).constData(), &entry, AT_SYMLINK_NOFOLLOW)
                        == 0
                    && S_ISREG(entry.st_mode) && same(entry, m->published.at(id))) {
                    ::unlinkat(sidecars, QFile::encodeName(name).constData(), 0);
                    m->published.erase(id);
                }
            }
            ::fsync(sidecars);
        }
        return true;
    } catch (const std::exception& exception) {
        if (writing) {
            m->fail(exception);
            error = m->error;
        } else
            error = QStringLiteral("Cannot save this batch: %1").arg(QString::fromUtf8(exception.what()));
        return false;
    }
}
