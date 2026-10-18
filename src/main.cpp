// omaaudio — OmaAudio. Qt Quick (QML) UI, C++ backend.

#include <QGuiApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QUrl>
#include <QScopeGuard>
#include <cstdio>

#include "backend.h"
#include "videoframeitem.h"
#include "keywatcher.h"
#include "cpuaudioview.h"
#include "commandline.h"
#include "agentcli.h"
#include "agentskill.h"
#include "storagepaths.h"
#include "graphicsstartup.h"
#include "theme.h"
#include "uitheme.h"

#include <pipewire/pipewire.h>

#ifdef OMAAUDIO_STARTUP_TEST_DRIVER
void attachStartupTestDriver(Backend *, QQuickWindow *, bool);
#endif

namespace {
void setApplicationIdentity(QCoreApplication &app)
{
    // One name for every XDG role (see storagepaths.h). The organisation used to
    // be "omawave"; settings written under it are copied across once, below.
    app.setOrganizationName(QStringLiteral("omaaudio"));
    app.setApplicationName(QStringLiteral("omaaudio"));
    app.setApplicationVersion(QString::fromLatin1(omaaudio::cli::version));
}

// Before any QSettings or the keymap is read: bring the earlier omawave settings
// across when OmaAudio has none of its own. Nothing is deleted or rewritten.
void migrateLegacySettings()
{
    namespace storage = omaaudio::storage;
    const auto result = storage::migrateLegacyConfig(storage::locations());
    storage::setLaunchMigration(result);
    if (!result.summary().isEmpty())
        qInfo().noquote() << result.summary();
}

int printCommandLineResult(const omaaudio::cli::Request &request)
{
    auto text = request.message.toLocal8Bit();
    if (!text.endsWith('\n')) text.append('\n');
    std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()),
                request.action == omaaudio::cli::Action::Error ? stderr : stdout);
    return request.action == omaaudio::cli::Action::Error ? 2 : 0;
}
}

namespace {
int runApplication(int &argc, char *argv[], const QStringList &rawArguments,
                   QString &executable, QString &checkpoint, bool &restart) {
    namespace graphics = omaaudio::graphics;
    QGuiApplication app(argc, argv);
    setApplicationIdentity(app);
    migrateLegacySettings();
    executable = QCoreApplication::applicationFilePath();
    if (qEnvironmentVariable(graphics::probeVariable) == "1") {
        qunsetenv(graphics::probeVariable);
        return graphics::probeVulkan();
    }
    const bool restarted = qEnvironmentVariable(graphics::restartVariable) == "1";
    const QString recovery = QString::fromUtf8(qgetenv(graphics::recoveryVariable));
    qunsetenv(graphics::restartVariable);
    qunsetenv(graphics::recoveryVariable);
    // QGuiApplication has consumed its own options, such as -platform.
    const auto request = omaaudio::cli::parse(app.arguments());
    if (request.action != omaaudio::cli::Action::Run) return printCommandLineResult(request);

    const auto choice = graphics::startupChoice(QProcessEnvironment::systemEnvironment(),
                                                app.platformName(), restarted);
    if (choice == graphics::Choice::Software) graphics::useSoftware();
    else if (choice == graphics::Choice::ProbeVulkan) {
        QString diagnostic;
        if (graphics::runProbe(executable, rawArguments.mid(1),
                               QProcessEnvironment::systemEnvironment(), diagnostic)) {
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        } else {
            qWarning().noquote() << diagnostic;
            graphics::useSoftware();
        }
    }
    // Probe and renderer selection precede every audio object.
    pw_init(nullptr, nullptr);
    const auto pipewireCleanup = qScopeGuard([] { pw_deinit(); });

    // The identity trio. setDesktopFileName is what becomes the Wayland app_id,
    // which is what Hyprland rules, the taskbar and the icon lookup key off.
    app.setDesktopFileName(QStringLiteral("omaaudio"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omaaudio")));

    // Basic, not Material: every control is drawn by the Oma kit in src/ui, and the
    // Material style would add ripples, elevation and its own radii under them.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    OmarchyTheme theme;
    Backend backend;

    // `Style` under the name the ported QML still uses -- see uitheme.h for why it
    // exists at all. Declared BEFORE the engine on purpose: destruction runs in
    // reverse, so anything the QML binds to has to outlive the engine, or closing
    // the window floods stderr with hundreds of "Cannot read property 'font' of
    // null" as every binding re-evaluates against a dead object during teardown.
    // ⚠️ The same holds for `theme` above, which the palette singleton binds to.
    Styles styles(&theme);

    // Carry the desktop's text size into the default font so every control
    // grows with `omarchy display text size`, without a restart.
    const qreal basePointSize = app.font().pointSizeF();
    const auto scaleFont = [&app, basePointSize](qreal scale) {
        QFont font = app.font();
        font.setPointSizeF(basePointSize * scale);
        app.setFont(font);
    };
    scaleFont(theme.textScale());
    QObject::connect(&theme, &OmarchyTheme::textScaleChanged, &app, [&theme, scaleFont] {
        scaleFont(theme.textScale());
    });

    qmlRegisterType<VideoFrameItem>("OmaAudio.Video", 1, 0, "VideoFrame");
    // ⚠️ A module singleton, not another context property: `src/ui/Theme.qml` is a
    // `pragma Singleton` behind a qmldir, and a QML singleton cannot see context
    // properties. This is the only way the palette can read the desktop's accent.
    qmlRegisterSingletonInstance("OmaAudio.Desktop", 1, 0, "OmarchyTheme", &theme);
    auto engineOwner = std::make_unique<QQmlApplicationEngine>();
    auto &engine = *engineOwner;
    const auto displayRenderer = std::make_shared<CpuAudioRenderer>();
    engine.addImageProvider(QStringLiteral("audio-cpu"), new CpuAudioViewProvider(backend.displayCache(), displayRenderer));
    engine.addImageProvider(QStringLiteral("audio-texture"), new AudioTextureProvider(backend.displayCache(), displayRenderer));
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    KeyWatcher keyWatcher(&app);
    engine.rootContext()->setContextProperty(QStringLiteral("keyWatcher"), &keyWatcher);

    engine.rootContext()->setContextProperty(QStringLiteral("Style"), &styles);

    // Before the window: a failure here is a message the window has to be able to
    // show, not a reason to refuse to open.
    backend.start();

    if (restarted && !recovery.isEmpty()) (void)backend.offerGraphicsRecovery(recovery);
    // beginCreate leaves bindings and component completion pending. Install the
    // guard before visible:true can create the first scene graph.
    auto component = std::make_unique<QQmlComponent>(&engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    std::unique_ptr<QObject> root(component->beginCreate(engine.rootContext()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    if (!window) {
        qCritical() << "Could not load the OmaAudio interface." << component->errors();
        if (root) component->completeCreate();
        return 1;
    }
    QQmlEngine::setObjectOwnership(root.get(), QQmlEngine::CppOwnership);
    engine.setIncubationController(window->incubationController());
    graphics::GraphicsGuard guard(window);
    bool graphicsFailed = false;
    QObject::connect(&guard, &graphics::GraphicsGuard::failed, &app,
                     [&](const QString &message) {
        graphicsFailed = true;
        restart = !restarted && window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
        qCritical().noquote() << "OmaAudio graphics failure:" << message;
        app.exit(1);
    });
#ifdef OMAAUDIO_STARTUP_TEST_DRIVER
    if (!restarted && qgetenv("OMAAUDIO_TEST_SCENARIO") == "early-error")
        window->sceneGraphError(QQuickWindow::ContextNotAvailable, "injected first-frame failure");
#endif
    root->setProperty("graphicsRestarted", restarted);
    component->completeCreate();
    if (component->isError()) {
        guard.disarm();
        qCritical() << "Could not complete the OmaAudio interface." << component->errors();
        return 1;
    }

    // "Open with", and `omaaudio somefile` from a terminal.
    if ((!restarted || recovery.isEmpty()) && !request.path.isEmpty()) {
        if (request.project) backend.loadProject(request.path);
        else backend.open(request.path);
    }

#ifdef OMAAUDIO_STARTUP_TEST_DRIVER
    attachStartupTestDriver(&backend, window, restarted);
#endif

    // QML completion may briefly process events. A failure delivered there must
    // not be forgotten merely because the main event loop has not started yet.
    const int result = graphicsFailed ? 1 : app.exec();
    guard.disarm();
    // QML and providers finish before the Backend checkpoint quiesces audio and
    // workers. All objects below unwind before main replaces this process.
    root.reset();
    component.reset();
    engineOwner.reset();
    if (graphicsFailed && !backend.prepareGraphicsRestart(checkpoint)) {
        restart = false;
        qCritical().noquote() << backend.lastError();
        return 1;
    }
    if (graphicsFailed && !restart && !checkpoint.isEmpty())
        qCritical().noquote() << "The session is preserved for the next launch:" << checkpoint;
    return graphicsFailed ? 1 : result;
}
} // namespace

int main(int argc, char *argv[]) {
    QStringList rawArguments;
    for (int i = 0; i < argc; ++i) rawArguments.append(QString::fromLocal8Bit(argv[i]));
    if (omaaudio::skill::requested(rawArguments)) {
        QCoreApplication app(argc, argv);
        setApplicationIdentity(app);
        return omaaudio::skill::run(rawArguments);
    }
    if (omaaudio::agent::requested(rawArguments)) return omaaudio::agent::run(argc, argv, rawArguments);
    if (omaaudio::cli::informationRequested(rawArguments)) {
        QCoreApplication app(argc, argv);
        setApplicationIdentity(app);
        return printCommandLineResult(omaaudio::cli::parse(app.arguments()));
    }
    QString executable, checkpoint;
    bool restart = false;
    const int result = runApplication(argc, argv, rawArguments, executable, checkpoint, restart);
    return restart ? omaaudio::graphics::restartInSoftware(executable, rawArguments.mid(1), checkpoint)
                   : result;
}
