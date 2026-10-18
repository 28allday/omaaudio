// Recipes: a named list of steps run in order on the selection or the focused
// track, each written into the audio, the whole run one Undo step.
//
// A step is a plain map, {kind, on, ...settings}, so a recipe can be stored,
// edited in the panel and sent over the command line alike. Most steps are an
// effect applied to the range through the same path as Effects › Apply (our own
// low cut, hum, level and peak limiter, the Sound tab's fixes, or any installed
// effect); Voice Cleanup, Room echo, click repair and fades use their own edits.
// Loudness to target measures the range first and applies the level that reaches
// it.
//
// The runner starts one step, waits on a short timer until nothing is busy, then
// judges it by the error it left and whether the history moved. A step that
// cannot run is reported and the others carry on. At the end the run's history
// entries are folded into one ("run <recipe>"). Stop cancels the step in progress
// and undoes what the run did. The run's edits are neither recorded as steps nor
// remembered for Apply Again.
//
// Own recipes are kept in the application's settings (category "recipes") for
// every project; the built-in ones are defined here and cannot be changed.
#include "backend.h"
#include "shortname.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

QVariantMap step(const QString& kind, QVariantMap settings = {})
{
    settings.insert(QStringLiteral("kind"), kind);
    settings.insert(QStringLiteral("on"), true);
    return settings;
}

struct BuiltIn { const char* id; const char* name; QVariantList steps; };

const std::vector<BuiltIn>& builtIns()
{
    static const std::vector<BuiltIn> list{
        {"builtin-podcast-voice", QT_TRANSLATE_NOOP("Recipes", "Podcast voice"), {
            step("lowcut", {{"frequency", 80}}), step("hum", {{"mains", 50}}),
            step("voiceCleanup", {{"amount", 60}}), step("sibilance", {{"amount", 35}}),
            step("evenout", {{"amount", 50}}),
            [] { auto s = step("roomEcho", {{"amount", 40}}); s["on"] = false; return s; }(),
            step("loudness", {{"lufs", -16}}), step("limit", {{"ceiling", -1}})}},
        {"builtin-phone-recording", QT_TRANSLATE_NOOP("Recipes", "Clean up a phone recording"), {
            step("lowcut", {{"frequency", 120}}), step("voiceCleanup", {{"amount", 70}}),
            step("clicks", {{"sensitivity", 50}}), step("evenout", {{"amount", 60}}),
            step("presence", {{"amount", 40}}), step("loudness", {{"lufs", -16}}), step("limit", {{"ceiling", -1}})}},
        {"builtin-music-bed", QT_TRANSLATE_NOOP("Recipes", "Music bed under speech"), {
            step("loudness", {{"lufs", -24}}), step("limit", {{"ceiling", -1}}),
            step("fadeIn", {{"seconds", 2}}), step("fadeOut", {{"seconds", 3}})}},
    };
    return list;
}

// One setting of a kind of step, as the panel shows it.
QVariantMap setting(const QString& key, const QString& label, double from, double to, double stepSize,
                    const QString& unit, double value)
{
    return {{"key", key}, {"label", label}, {"from", from}, {"to", to}, {"step", stepSize}, {"unit", unit}, {"default", value}};
}

double number(const QVariantMap& step, const char* key, double fallback)
{
    bool ok = false;
    const double value = step.value(QLatin1String(key)).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

QString signedDb(double db)
{
    return db > 0 ? QStringLiteral("+%1 dB").arg(db, 0, 'f', 1)
                  : QStringLiteral("%1 dB").arg(db, 0, 'f', 1).replace(QLatin1Char('-'), QChar(0x2212));
}

QString minus(QString text) { return text.replace(QLatin1Char('-'), QChar(0x2212)); }

}  // namespace

// --- the recipes ------------------------------------------------------------------

QVariantList Backend::ownRecipes() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("recipes"));
    const auto json = QJsonDocument::fromJson(settings.value(QStringLiteral("own")).toByteArray());
    return json.isArray() ? json.array().toVariantList() : QVariantList{};
}

void Backend::storeOwnRecipes(const QVariantList& list)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("recipes"));
    settings.setValue(QStringLiteral("own"), QJsonDocument(QJsonArray::fromVariantList(list)).toJson(QJsonDocument::Compact));
    settings.sync();
    emit recipesChanged();
}

QVariantList Backend::recipes() const
{
    QVariantList out;
    const auto describe = [&](QVariantMap recipe) {
        QStringList titles;
        const auto steps = recipe.value("steps").toList();
        for (const auto& s : steps) if (s.toMap().value("on", true).toBool()) titles << stepTitle(s.toMap());
        recipe["summary"] = (steps.size() == 1 ? tr("1 step") : tr("%1 steps").arg(steps.size())) + (titles.isEmpty() ? QString() : QStringLiteral(" · ") + titles.join(QStringLiteral(", ")));
        out.append(recipe);
    };
    for (const auto& b : builtIns())
        describe({{"id", QString::fromLatin1(b.id)}, {"name", QCoreApplication::translate("Recipes", b.name)}, {"builtIn", true}, {"steps", b.steps}});
    for (const auto& own : ownRecipes()) {
        auto recipe = own.toMap();
        recipe["builtIn"] = false;
        describe(recipe);
    }
    return out;
}

QVariantMap Backend::recipe(const QString& id) const
{
    for (const auto& r : recipes()) if (r.toMap().value("id").toString() == id) return r.toMap();
    return {};
}

QString Backend::saveRecipe(const QVariantMap& recipe)
{
    const auto name = recipe.value("name").toString().trimmed();
    if (name.isEmpty()) { setError(tr("Give the recipe a name.")); return {}; }
    const auto steps = recipe.value("steps").toList();
    const auto kinds = recipeStepKinds();
    for (const auto& s : steps) {
        const auto kind = s.toMap().value("kind").toString();
        const bool known = kind == QLatin1String("effect")
            ? !s.toMap().value("uri").toString().isEmpty()
            : std::any_of(kinds.begin(), kinds.end(), [&](const QVariant& k) { return k.toMap().value("kind").toString() == kind; });
        if (!known) { setError(tr("A step of an unknown kind (%1) cannot be saved.").arg(kind)); return {}; }
    }
    auto list = ownRecipes();
    auto id = recipe.value("id").toString();
    auto at = std::find_if(list.begin(), list.end(), [&](const QVariant& r) { return r.toMap().value("id").toString() == id; });
    if (id.isEmpty() || at == list.end()) {
        id = QStringLiteral("own-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        list.append(QVariantMap{{"id", id}, {"name", name}, {"steps", steps}});
    } else {
        *at = QVariantMap{{"id", id}, {"name", name}, {"steps", steps}};
    }
    storeOwnRecipes(list);
    setError({});
    return id;
}

QString Backend::duplicateRecipe(const QString& id)
{
    const auto original = recipe(id);
    if (original.isEmpty()) { setError(tr("There is no such recipe.")); return {}; }
    return saveRecipe({{"name", tr("%1 (copy)").arg(original.value("name").toString())}, {"steps", original.value("steps")}});
}

bool Backend::renameRecipe(const QString& id, const QString& name)
{
    auto list = ownRecipes();
    auto at = std::find_if(list.begin(), list.end(), [&](const QVariant& r) { return r.toMap().value("id").toString() == id; });
    if (at == list.end()) { setError(tr("Only your own recipes can be renamed.")); return false; }
    if (name.trimmed().isEmpty()) { setError(tr("Give the recipe a name.")); return false; }
    auto r = at->toMap(); r["name"] = name.trimmed(); *at = r;
    storeOwnRecipes(list);
    setError({});
    return true;
}

bool Backend::removeRecipe(const QString& id)
{
    auto list = ownRecipes();
    const auto before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const QVariant& r) { return r.toMap().value("id").toString() == id; }), list.end());
    if (list.size() == before) { setError(tr("Only your own recipes can be deleted.")); return false; }
    storeOwnRecipes(list);
    setError({});
    return true;
}

// --- the kinds of step -------------------------------------------------------------

QVariantList Backend::recipeStepKinds()
{
    const bool catalogue = !m_scanning && m_lv2->scanned();
    const auto plugin = [&](const char* uri, const char* name) -> QString {
        if (!catalogue) return tr("Reading the installed effects…");
        const auto* info = m_lv2->find(uri);
        if (!info || !info->usable) return tr("Needs the optional %1 plugin.").arg(QString::fromLatin1(name));
        return {};
    };
    const auto kind = [&](const char* id, const char* group, const QString& title, QVariantList settings, const QString& reason = {}) {
        const QString groups[] = {tr("Clean up"), tr("Level"), tr("Tone"), tr("Timing")};
        const QLatin1String key(group);
        const int g = key == QLatin1String("clean") ? 0 : key == QLatin1String("level") ? 1 : key == QLatin1String("tone") ? 2 : 3;
        return QVariantMap{{"kind", QString::fromLatin1(id)}, {"group", QString::fromLatin1(group)}, {"groupTitle", groups[g]},
            {"title", title}, {"settings", settings}, {"available", reason.isEmpty()}, {"reason", reason}};
    };
    const auto amount = [&](double value, const QString& low = {}, const QString& high = {}) {
        auto s = setting("amount", tr("How much"), 0, 100, 5, "%", value);
        s["lowText"] = low.isEmpty() ? tr("A little") : low;
        s["highText"] = high.isEmpty() ? tr("A lot") : high;
        return QVariantList{s};
    };
    QVariantList hum{setting("mains", tr("Mains"), 50, 60, 10, "Hz", 50)};
    hum[0] = [&] { auto s = hum[0].toMap(); s["choices"] = QVariantList{QVariantMap{{"value", 50}, {"label", tr("50 Hz")}},
                                                                           QVariantMap{{"value", 60}, {"label", tr("60 Hz")}}}; return s; }();
    // Shorten pauses: the other tracks stay, or move too (then the other voice
    // tracks must be quiet as well, so no one's words are cut). Music never moves.
    auto others = setting("moveOthers", tr("Other tracks"), 0, 1, 1, {}, 0);
    others["choices"] = QVariantList{QVariantMap{{"value", 0}, {"label", tr("Stay")}}, QVariantMap{{"value", 1}, {"label", tr("Move too")}}};
    return {
        kind("lowcut", "clean", tr("Low cut"), {setting("frequency", tr("Below"), 20, 300, 5, "Hz", 80)}, plugin("urn:omaaudio:lowcut", "OmaAudio Low Cut")),
        kind("hum", "clean", tr("Mains hum"), hum, plugin("urn:omaaudio:hum", "OmaAudio Hum Removal")),
        kind("voiceCleanup", "clean", tr("Voice Cleanup"), amount(60)),
        kind("roomEcho", "clean", tr("Room echo"), amount(40)),
        kind("clicks", "clean", tr("Find and Repair Clicks"), {setting("sensitivity", tr("Sensitivity"), 0, 100, 5, "%", 50)}),
        kind("loudness", "level", tr("Loudness to target"), {setting("lufs", tr("Target"), -30, -10, 1, "LUFS", -16)}, plugin("urn:omaaudio:gain", "OmaAudio Change Level")),
        kind("limit", "level", tr("Limit peaks"), {setting("ceiling", tr("No higher than"), -12, 0, 0.5, "dB", -1)}, plugin("urn:omaaudio:limit", "OmaAudio Limit Peaks")),
        kind("evenout", "level", tr("Even out"), amount(50, tr("Natural"), tr("Tight")), plugin("http://calf.sourceforge.net/plugins/Compressor", "Calf Compressor")),
        kind("level", "level", tr("Change level"), {setting("db", tr("By"), -24, 12, 0.5, "dB", 0)}, plugin("urn:omaaudio:gain", "OmaAudio Change Level")),
        kind("fadeIn", "level", tr("Fade in"), {setting("seconds", tr("Over"), 0.1, 10, 0.1, "s", 1)}),
        kind("fadeOut", "level", tr("Fade out"), {setting("seconds", tr("Over"), 0.1, 10, 0.1, "s", 2)}),
        kind("sibilance", "tone", tr("Harsh “s”"), amount(35, tr("Light"), tr("Strong")), plugin("http://calf.sourceforge.net/plugins/Deesser", "Calf Deesser")),
        kind("presence", "tone", tr("Presence"), amount(30, tr("Soft"), tr("Bright")), plugin("http://calf.sourceforge.net/plugins/Exciter", "Calf Exciter")),
        kind("width", "tone", tr("Width"), {setting("amount", tr("Width"), 0, 200, 5, "%", 100)}, plugin("urn:omaaudio:width", "OmaAudio Width")),
        kind("pauses", "timing", tr("Shorten pauses"), {
            setting("shortenTo", tr("Shorten each to"), 0, 3, 0.1, "s", 0.4),
            setting("minSeconds", tr("Pauses longer than"), 0.2, 5, 0.1, "s", 0.5), others}),
    };
}

QVariantMap Backend::effectStep(const QString& uri)
{
    if (!m_scanning && !m_lv2->scanned()) m_lv2->scan();
    const auto* info = m_lv2->find(uri.toStdString());
    if (!info || !info->usable) { setError(tr("That effect is not installed or cannot be used.")); return {}; }
    setError({});
    return {{"kind", "effect"}, {"on", true}, {"uri", uri}, {"name", QString::fromStdString(info->name)}, {"controls", QVariantList{}}};
}

QString Backend::stepTitle(const QVariantMap& step) const
{
    const auto kind = step.value("kind").toString();
    static const QMap<QString, const char*> titles{
        {"lowcut", QT_TR_NOOP("Low cut")}, {"hum", QT_TR_NOOP("Mains hum")}, {"voiceCleanup", QT_TR_NOOP("Voice Cleanup")},
        {"roomEcho", QT_TR_NOOP("Room echo")}, {"clicks", QT_TR_NOOP("Find and Repair Clicks")},
        {"loudness", QT_TR_NOOP("Loudness to target")}, {"limit", QT_TR_NOOP("Limit peaks")}, {"evenout", QT_TR_NOOP("Even out")},
        {"level", QT_TR_NOOP("Change level")}, {"fadeIn", QT_TR_NOOP("Fade in")}, {"fadeOut", QT_TR_NOOP("Fade out")},
        {"sibilance", QT_TR_NOOP("Harsh “s”")}, {"presence", QT_TR_NOOP("Presence")}, {"width", QT_TR_NOOP("Width")},
        {"pauses", QT_TR_NOOP("Shorten pauses")}};
    if (kind == QLatin1String("effect")) {
        const auto name = step.value("name").toString();
        return name.isEmpty() ? step.value("uri").toString() : name;
    }
    return titles.contains(kind) ? tr(titles.value(kind)) : kind;
}

QString Backend::stepSummary(const QVariantMap& step) const
{
    const auto kind = step.value("kind").toString();
    const auto percent = [&](const char* key, double fallback) { return QStringLiteral("%1 %").arg(std::lround(number(step, key, fallback))); };
    QString text;
    if (kind == QLatin1String("lowcut")) text = tr("Below %1 Hz").arg(std::lround(number(step, "frequency", 80)));
    else if (kind == QLatin1String("hum")) text = tr("%1 Hz").arg(number(step, "mains", 50) >= 55 ? 60 : 50);
    else if (kind == QLatin1String("clicks")) text = tr("Sensitivity %1").arg(percent("sensitivity", 50));
    else if (kind == QLatin1String("loudness")) text = minus(tr("%1 LUFS").arg(number(step, "lufs", -16), 0, 'f', 0));
    else if (kind == QLatin1String("limit")) text = minus(tr("No higher than %1 dB").arg(number(step, "ceiling", -1), 0, 'g', 3));
    else if (kind == QLatin1String("level")) text = signedDb(number(step, "db", 0));
    else if (kind == QLatin1String("fadeIn") || kind == QLatin1String("fadeOut"))
        text = tr("%1 s").arg(number(step, "seconds", kind == QLatin1String("fadeIn") ? 1 : 2), 0, 'g', 3);
    else if (kind == QLatin1String("width")) text = percent("amount", 100);
    else if (kind == QLatin1String("pauses"))
        text = tr("Pauses over %1 s become %2 s").arg(number(step, "minSeconds", 0.5), 0, 'g', 3).arg(number(step, "shortenTo", 0.4), 0, 'g', 3)
             + (number(step, "moveOthers", 0) >= 0.5 ? tr(" · others move") : QString());
    else if (kind == QLatin1String("effect")) {
        auto count = step.value("controls").toList().size();
        for (auto it = step.cbegin(); it != step.cend(); ++it) count += it.key().startsWith(QLatin1String("c:"));
        text = count == 1 ? tr("1 setting") : count ? tr("%1 settings").arg(count) : tr("Its default settings");
    } else text = percent("amount", 50);
    return step.value("on", true).toBool() ? text : tr("Off · %1").arg(text);
}

// --- recording steps ------------------------------------------------------------------

void Backend::startRecordingSteps()
{
    m_recordedSteps.clear();
    m_recordingSteps = true;
    emit recordedStepsChanged();
}

QVariantList Backend::stopRecordingSteps(bool keep)
{
    const auto steps = keep ? m_recordedSteps : QVariantList{};
    m_recordingSteps = false;
    if (!keep) m_recordedSteps.clear();
    emit recordedStepsChanged();
    return steps;
}

void Backend::recordStep(const QVariantMap& step)
{
    if (!m_recordingSteps || m_recipeApplying) return;
    m_recordedSteps.append(step);
    emit recordedStepsChanged();
}

// --- effects for steps ----------------------------------------------------------------

std::optional<session::Insert> Backend::recipeInsert(const QString& uri, const std::map<std::string, double>& bySymbol, QString& error)
{
    if (!m_scanning && !m_lv2->scanned()) m_lv2->scan();
    const auto* info = m_lv2->find(uri.toStdString());
    if (!info || !info->usable) { error = tr("The effect it needs (%1) is not installed.").arg(uri); return std::nullopt; }
    if (m_pluginLibrary.disabled().contains(uri)) { error = tr("%1 is disabled in the Plug-in Manager.").arg(QString::fromStdString(info->name)); return std::nullopt; }
    std::string failure;
    auto probe = host::createInstance(*m_lv2, uri.toStdString(), hostFeatures(), failure);
    if (!probe) { error = QString::fromStdString(failure); return std::nullopt; }
    session::Insert insert;
    insert.uri = uri.toStdString();
    for (const auto& [symbol, value] : bySymbol) {
        const auto p = std::find_if(probe->controls().begin(), probe->controls().end(),
            [&](const auto& c) { return !c.output && c.symbol == symbol; });
        if (p == probe->controls().end()) { error = tr("The effect has no setting called %1.").arg(QString::fromStdString(symbol)); return std::nullopt; }
        double raw = p->clamp(value);
        if (p->integer || p->toggled) raw = std::round(raw);
        if (raw != p->defaultValue) insert.setControl(p->index, raw);
    }
    return insert;
}

// The effect a step applies, or "" for a step that is not an effect. On failure the
// reason, with `name` empty.
QString Backend::recipeStepInsert(const QVariantMap& step, session::Insert& insert, QString& name)
{
    const auto kind = step.value("kind").toString();
    QString error;
    std::optional<session::Insert> made;
    if (kind == QLatin1String("lowcut"))
        made = recipeInsert(QStringLiteral("urn:omaaudio:lowcut"), {{"frequency", number(step, "frequency", 80)}}, error);
    else if (kind == QLatin1String("hum"))
        made = recipeInsert(QStringLiteral("urn:omaaudio:hum"), {{"mains", number(step, "mains", 50) >= 55 ? 60 : 50}, {"harmonics", 4}}, error);
    else if (kind == QLatin1String("limit"))
        made = recipeInsert(QStringLiteral("urn:omaaudio:limit"), {{"ceiling", number(step, "ceiling", -1)}}, error);
    else if (kind == QLatin1String("level"))
        made = recipeInsert(QStringLiteral("urn:omaaudio:gain"), {{"gain", number(step, "db", 0)}}, error);
    else if (kind == QLatin1String("evenout") || kind == QLatin1String("sibilance") || kind == QLatin1String("presence"))
        made = soundFixInsert(kind, number(step, "amount", 50) / 100, 50, error);
    else if (kind == QLatin1String("width"))
        made = soundFixInsert(kind, number(step, "amount", 100) / 200, 50, error);
    else if (kind == QLatin1String("effect")) {
        const auto uri = step.value("uri").toString();
        std::map<std::string, double> controls;
        std::vector<std::pair<std::uint32_t, double>> byIndex;
        for (const auto& c : step.value("controls").toList()) {
            const auto m = c.toMap();
            if (!m.value("symbol").toString().isEmpty()) controls[m.value("symbol").toString().toStdString()] = m.value("value").toDouble();
            else byIndex.emplace_back(m.value("index").toUInt(), m.value("value").toDouble());
        }
        // Settings changed in the editor ("c:<symbol>") win over recorded ones.
        for (auto it = step.cbegin(); it != step.cend(); ++it)
            if (it.key().startsWith(QLatin1String("c:"))) controls[it.key().mid(2).toStdString()] = it.value().toDouble();
        made = recipeInsert(uri, controls, error);
        if (made) for (const auto& [index, value] : byIndex) made->setControl(index, value);
        else if (error.contains(uri) && !step.value("name").toString().isEmpty())
            error = tr("%1 is not installed on this computer.").arg(step.value("name").toString());
    } else return {};
    if (!made) return error;
    insert = std::move(*made);
    name = stepTitle(step);
    return {};
}

// --- running -------------------------------------------------------------------------

QVariantMap Backend::recipeTarget(const QString& target) const
{
    const auto* track = m_project.track(m_focused);
    if (!track) return {{"valid", false}, {"reason", tr("Click a track first.")}};
    const auto name = shortName(QString::fromStdString(track->name));
    if (target == QLatin1String("selection")) {
        if (m_selection.empty() || m_selection.track != m_focused)
            return {{"valid", false}, {"reason", tr("Select a time range on a track first: drag across its waveform.")}};
        if (m_selection.channel >= 0)
            return {{"valid", false}, {"reason", tr("Select both channels: a recipe works on the whole sound of the range.")}};
        return {{"valid", true}, {"start", qint64(m_selection.start)}, {"end", qint64(m_selection.end)},
                {"text", tr("%1 – %2 on %3").arg(formatTime(m_selection.start), formatTime(m_selection.end), name)}};
    }
    if (target == QLatin1String("track")) {
        if (track->clips.empty()) return {{"valid", false}, {"reason", tr("%1 has no audio.").arg(name)}};
        session::FramePos start = track->clips.front().timelineStart, end = 0;
        for (const auto& c : track->clips) { start = std::min(start, c.timelineStart); end = std::max(end, c.timelineEnd()); }
        return {{"valid", true}, {"start", qint64(start)}, {"end", qint64(end)},
                {"text", (track->clips.size() == 1 ? tr("On %1, the whole track (1 clip)").arg(name)
                                                  : tr("On %1, the whole track (%2 clips)").arg(name).arg(track->clips.size()))}};
    }
    return {{"valid", false}, {"reason", tr("Choose the selection or this track.")}};
}

bool Backend::recipeBusy() const
{
    return sampleEditing() || m_repairing || m_detecting || m_autoHealPending;
}

bool Backend::runRecipe(const QString& id, const QString& target)
{
    if (m_recipeRunner) { setError(tr("A recipe is already running.")); return false; }
    const auto chosen = recipe(id);
    if (chosen.isEmpty()) { setError(tr("There is no such recipe.")); return false; }
    if (m_shuttingDown || m_loading || recording() || automationWriting() || recipeBusy() || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current edit before running a recipe.")); return false;
    }
    if (m_effectPreview && m_effectPreview->generation == m_sessionGeneration) {
        setError(tr("Apply or cancel the effect being previewed first.")); return false;
    }
    if (m_scanning) { setError(tr("Wait a moment: the installed effects are still being read.")); return false; }
    const auto range = recipeTarget(target);
    if (!range.value("valid").toBool()) { setError(range.value("reason").toString()); return false; }
    const auto steps = chosen.value("steps").toList();
    if (std::none_of(steps.begin(), steps.end(), [](const QVariant& s) { return s.toMap().value("on", true).toBool(); })) {
        setError(tr("Every step of this recipe is off.")); return false;
    }
    if (!m_scanning && !m_lv2->scanned()) m_lv2->scan();

    RecipeRunner run;
    run.id = id; run.name = chosen.value("name").toString(); run.target = target;
    run.targetText = range.value("text").toString();
    run.track = m_focused; run.start = range.value("start").toLongLong(); run.end = range.value("end").toLongLong();
    run.selectionBefore = m_selection; run.focusBefore = m_focused;
    run.undoVersion = m_undo.version();
    run.steps = steps;
    run.runClock.start();
    m_recipeRunner = std::move(run);
    m_recipeApplying = true;
    m_lastRecipeId = id;

    QVariantList rows;
    for (const auto& s : steps) {
        const auto m = s.toMap();
        rows.append(QVariantMap{{"title", stepTitle(m)}, {"summary", stepSummary(m)},
            {"state", m.value("on", true).toBool() ? "waiting" : "off"}, {"seconds", 0.0}, {"result", QString()},
            {"reason", QString()}, {"progress", 0.0}});
    }
    m_recipeRunView = {{"active", true}, {"running", true}, {"finished", false}, {"stopped", false},
        {"id", id}, {"name", m_recipeRunner->name}, {"target", target}, {"targetText", m_recipeRunner->targetText},
        {"current", 0}, {"total", int(std::count_if(steps.begin(), steps.end(), [](const QVariant& s) { return s.toMap().value("on", true).toBool(); }))},
        {"done", 0}, {"failed", 0}, {"seconds", 0.0}, {"undoLabel", QString()}, {"steps", rows}};
    if (!m_recipeTimer) {
        m_recipeTimer = new QTimer(this);
        m_recipeTimer->setInterval(30);
        connect(m_recipeTimer, &QTimer::timeout, this, &Backend::recipeTick);
    }
    setError({});
    emit recipeRunChanged();
    startRecipeStep();
    if (m_recipeRunner) m_recipeTimer->start();
    return true;
}

bool Backend::runLastRecipeAgain()
{
    if (m_lastRecipeId.isEmpty() || recipe(m_lastRecipeId).isEmpty()) { setError(tr("Run a recipe first; this runs it again.")); return false; }
    return runRecipe(m_lastRecipeId, recipeTarget(QStringLiteral("selection")).value("valid").toBool()
                                         ? QStringLiteral("selection") : QStringLiteral("track"));
}

QString Backend::lastRecipeName() const
{
    return m_lastRecipeId.isEmpty() ? QString() : recipe(m_lastRecipeId).value("name").toString();
}

void Backend::publishRecipeStep(int index, const QVariantMap& changes)
{
    auto rows = m_recipeRunView.value("steps").toList();
    if (index < 0 || index >= rows.size()) return;
    auto row = rows[index].toMap();
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) row[it.key()] = it.value();
    rows[index] = row;
    m_recipeRunView["steps"] = rows;
}

void Backend::startRecipeStep()
{
    auto& run = *m_recipeRunner;
    do { ++run.index; } while (run.index < run.steps.size() && !run.steps[run.index].toMap().value("on", true).toBool());
    if (run.index >= run.steps.size()) { finishRecipe(false); return; }
    const auto step = run.steps[run.index].toMap();
    const auto kind = step.value("kind").toString();
    m_recipeRunView["current"] = m_recipeRunView.value("current").toInt() + 1;
    publishRecipeStep(run.index, {{"state", "running"}});
    emit recipeRunChanged();

    run.stepClock.start();
    run.stepVersion = m_undo.version();
    run.stepResult.clear();
    run.waiting = true;
    setError({});
    if (!m_project.track(run.track)) { endRecipeStep(false, {}, tr("The track is gone.")); return; }
    if (m_focused != run.track) focusTrack(int(run.track));
    const auto rate = double(m_project.sampleRate());
    auto from = run.start, to = run.end;
    if (kind == QLatin1String("fadeIn")) to = std::min(run.end, run.start + session::FramePos(std::llround(number(step, "seconds", 1) * rate)));
    if (kind == QLatin1String("fadeOut")) from = std::max(run.start, run.end - session::FramePos(std::llround(number(step, "seconds", 2) * rate)));
    select(from, to, false, int(run.track));
    if (m_selection.empty()) { endRecipeStep(false, {}, tr("The range to work on is empty.")); return; }

    if (kind == QLatin1String("voiceCleanup")) {
        const auto target = voiceCleanupTarget();
        if (!applyVoiceCleanup(std::clamp(number(step, "amount", 60), 1.0, 100.0), target.value("stamp").toString()))
            endRecipeStep(false, {}, m_lastError);
        return;
    }
    if (kind == QLatin1String("roomEcho")) {
        if (!reduceRoomEcho(int(run.track), std::clamp(number(step, "amount", 40) / 100, 0.0, 1.0)))
            endRecipeStep(false, {}, m_lastError);
        return;
    }
    if (kind == QLatin1String("clicks")) {
        autoHealClicks(std::clamp(number(step, "sensitivity", 50) / 100, 0.0, 1.0));
        if (!m_lastError.isEmpty()) endRecipeStep(false, {}, m_lastError);
        return;
    }
    if (kind == QLatin1String("pauses")) { runPauseStep(step); return; }
    if (kind == QLatin1String("fadeIn") || kind == QLatin1String("fadeOut")) {
        kind == QLatin1String("fadeIn") ? fadeIn() : fadeOut();
        if (!m_lastError.isEmpty()) endRecipeStep(false, {}, m_lastError);
        return;                          // judged on the next tick, like the rest
    }

    // An effect. Loudness to target is the level that reaches the target.
    auto effect = step;
    if (kind == QLatin1String("loudness")) {
        const auto stats = analyseAudio(QStringLiteral("statistics"));
        const double lufs = stats.value("integratedLufs").toDouble();
        if (!stats.contains("integratedLufs") || !std::isfinite(lufs) || lufs <= -70) {
            endRecipeStep(true, tr("Silent; nothing to change"), {}); return;
        }
        run.loudnessBefore = lufs;
        const double change = std::clamp(number(step, "lufs", -16) - lufs, -48.0, 24.0);
        if (std::abs(change) < 0.05) { endRecipeStep(true, minus(tr("%1 LUFS already").arg(lufs, 0, 'f', 1)), {}); return; }
        effect = {{"kind", "level"}, {"db", change}};
    }
    session::Insert insert;
    QString name;
    const auto failure = recipeStepInsert(effect, insert, name);
    if (!failure.isEmpty() || name.isEmpty()) { endRecipeStep(false, {}, failure.isEmpty() ? tr("Unknown step.") : failure); return; }
    insert.forkIdentity();
    EffectPreview target{.track = run.track, .selection = m_selection, .uri = QString::fromStdString(insert.uri), .name = name.toStdString()};
    startEffectApply(target, std::move(insert));
    if (!m_lastError.isEmpty()) endRecipeStep(false, {}, m_lastError);
}

void Backend::recipeTick()
{
    if (!m_recipeRunner || !m_recipeRunner->waiting || recipeBusy()) return;
    auto& run = *m_recipeRunner;
    if (run.stopping) { finishRecipe(true); return; }
    const auto step = run.steps[run.index].toMap();
    const auto kind = step.value("kind").toString();
    // The history says whether it worked: the last error is shared, and a refusal
    // of something else meanwhile (a second Run) must not fail a step that ran.
    const bool changed = m_undo.version() != run.stepVersion;
    if (!changed && !m_lastError.isEmpty()) { endRecipeStep(false, {}, m_lastError); return; }
    QString result;
    if (!run.stepResult.isEmpty() && changed) {
        result = run.stepResult;
    } else if (kind == QLatin1String("loudness") && changed) {
        const auto stats = analyseAudio(QStringLiteral("statistics"));
        result = minus(tr("%1 → %2 LUFS").arg(run.loudnessBefore, 0, 'f', 1).arg(stats.value("integratedLufs").toDouble(), 0, 'f', 1));
    } else if (kind == QLatin1String("limit") && changed) {
        const double peak = selectionPeak();
        result = minus(tr("Highest peak %1 dB").arg(peak > 0 ? 20 * std::log10(peak) : -120.0, 0, 'f', 1));
    } else if (kind == QLatin1String("clicks") && !changed) {
        result = tr("No clicks found");
    } else if (!changed) {
        result = tr("Nothing to change");
    }
    endRecipeStep(true, result, {});
}

void Backend::endRecipeStep(bool ok, const QString& result, const QString& reason)
{
    if (!m_recipeRunner) return;
    auto& run = *m_recipeRunner;
    run.waiting = false;
    const double seconds = run.stepClock.isValid() ? run.stepClock.elapsed() / 1000.0 : 0.0;
    publishRecipeStep(run.index, {{"state", ok ? "done" : "failed"}, {"seconds", seconds}, {"result", result},
                                  {"reason", ok ? QString() : (reason.isEmpty() ? tr("It could not run.") : reason)}, {"progress", 1.0}});
    m_recipeRunView[ok ? "done" : "failed"] = m_recipeRunView.value(ok ? "done" : "failed").toInt() + 1;
    m_recipeRunView["seconds"] = run.runClock.elapsed() / 1000.0;
    setError({});
    emit recipeRunChanged();
    // The next step from the event loop, so a step's own signals settle first.
    QTimer::singleShot(0, this, [this] { if (m_recipeRunner && !m_recipeRunner->waiting && !m_recipeRunner->stopping) startRecipeStep(); });
}

void Backend::stopRecipe()
{
    if (!m_recipeRunner) return;
    m_recipeRunner->stopping = true;
    if (sampleEditing()) cancelSampleEdit();
    if (m_repairing) cancelRepair();
    if (!recipeBusy()) finishRecipe(true);
}

void Backend::finishRecipe(bool stopped)
{
    if (!m_recipeRunner) return;
    auto run = std::move(*m_recipeRunner);
    m_recipeRunner.reset();
    if (m_recipeTimer) m_recipeTimer->stop();
    const auto label = tr("run %1").arg(run.name).toStdString();
    const bool folded = m_undo.foldSince(run.undoVersion, label);
    if (stopped && folded) {
        undo();
        m_undo.discardRedo();
    }
    m_recipeApplying = false;
    // Back to what was selected before, on the same track.
    if (m_project.track(run.focusBefore) && m_focused != run.focusBefore) focusTrack(int(run.focusBefore));
    if (run.selectionBefore.empty()) clearSelection();
    else select(run.selectionBefore.start, run.selectionBefore.end, false, int(run.selectionBefore.track));
    auto rows = m_recipeRunView.value("steps").toList();
    for (auto& r : rows) {
        auto row = r.toMap();
        // Stopped, nothing a step did is kept: they all read as not run.
        if (stopped && row.value("state") != QLatin1String("off")) { row["state"] = "waiting"; row["seconds"] = 0.0; row["result"] = QString(); }
        else if (row.value("state") == QLatin1String("running")) row["state"] = "failed";
        r = row;
    }
    m_recipeRunView["steps"] = rows;
    m_recipeRunView["running"] = false;
    m_recipeRunView["finished"] = !stopped;
    m_recipeRunView["stopped"] = stopped;
    m_recipeRunView["seconds"] = run.runClock.elapsed() / 1000.0;
    m_recipeRunView["undoLabel"] = folded && !stopped ? QString::fromStdString(label) : QString();
    emit timelineChanged();
    emit recipeRunChanged();
}

void Backend::clearRecipeRun()
{
    if (m_recipeRunner) return;
    m_recipeRunView.clear();
    emit recipeRunChanged();
}

// An effect applied to a selection, as a step with its settings: by symbol where
// the plugin names its controls, so the step survives a plugin update.
QVariantMap Backend::effectStepFor(const session::Insert& insert, const std::string& name)
{
    QVariantList controls;
    std::string error;
    auto probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error);
    QVariantMap out{{"kind", "effect"}, {"on", true}, {"uri", QString::fromStdString(insert.uri)},
                    {"name", QString::fromStdString(name)}};
    for (const auto& c : insert.controls) {
        QString symbol;
        if (probe)
            for (const auto& p : probe->controls())
                if (!p.output && p.index == c.port) { symbol = QString::fromStdString(p.symbol); break; }
        if (!symbol.isEmpty()) out[QStringLiteral("c:") + symbol] = c.value;
        else controls.append(QVariantMap{{"index", c.port}, {"value", c.value}});
    }
    out["controls"] = controls;
    return out;
}

QVariantList Backend::stepSettings(const QVariantMap& step)
{
    const auto kind = step.value("kind").toString();
    if (kind != QLatin1String("effect")) {
        for (const auto& k : recipeStepKinds())
            if (k.toMap().value("kind").toString() == kind) return k.toMap().value("settings").toList();
        return {};
    }
    if (!m_scanning && !m_lv2->scanned()) m_lv2->scan();
    std::string error;
    auto probe = host::createInstance(*m_lv2, step.value("uri").toString().toStdString(), hostFeatures(), error);
    if (!probe) return {};
    QVariantList out;
    for (const auto& p : probe->controls()) {
        if (p.output || p.symbol.empty() || !std::isfinite(p.minimum) || !std::isfinite(p.maximum) || p.maximum <= p.minimum) continue;
        auto s = setting(QStringLiteral("c:") + QString::fromStdString(p.symbol),
                         QString::fromStdString(p.name.empty() ? p.symbol : p.name), p.minimum, p.maximum,
                         p.integer || p.toggled ? 1 : 0, {}, p.defaultValue);
        if (p.toggled) s["choices"] = QVariantList{QVariantMap{{"value", 0}, {"label", tr("Off")}}, QVariantMap{{"value", 1}, {"label", tr("On")}}};
        out.append(s);
    }
    return out;
}
