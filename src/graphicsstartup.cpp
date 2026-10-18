#include "graphicsstartup.h"

#include <QEvent>
#include <QElapsedTimer>
#include <QProcess>
#include <QQuickWindow>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QWindow>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace omaaudio::graphics {
Choice startupChoice(const QProcessEnvironment &environment, const QString &platform,
                     bool restarting)
{
    if (restarting) return Choice::Software;
    if (!environment.value("QT_QUICK_BACKEND").isEmpty()
        || !environment.value("QSG_RHI_BACKEND").isEmpty()) return Choice::Explicit;
    const auto base = platform.section(':', 0, 0);
    if (base == "offscreen" || base == "minimal" || base == "vnc") return Choice::Software;
    return Choice::ProbeVulkan;
}

void useSoftware()
{
    qunsetenv("QSG_RHI_BACKEND");
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
}

int probeVulkan()
{
    // A faulty loader/driver can crash this child, but never opens a session or
    // audio stream. Do not generate a potentially large core for a startup probe.
    const rlimit noCore{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &noCore);
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 1));
    if (!instance.create()) return 1;
    auto *functions = instance.functions();
    uint32_t count = 0;
    if (functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr) != VK_SUCCESS
        || count == 0 || count > 64) return 1;
    std::vector<VkPhysicalDevice> devices(count);
    if (functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data()) != VK_SUCCESS)
        return 1;
    QWindow surface;
    surface.setSurfaceType(QSurface::VulkanSurface);
    surface.setVulkanInstance(&instance);
    surface.resize(16, 16);
    surface.create(); // Native surface only: never show or steal focus.
    if (!QVulkanInstance::surfaceForWindow(&surface)) return 1;
    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDeviceProperties properties{};
        functions->vkGetPhysicalDeviceProperties(devices[d], &properties);
        // A CPU Vulkan implementation does not establish hardware availability.
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t families = 0;
        functions->vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, nullptr);
        if (families == 0 || families > 256) continue;
        std::vector<VkQueueFamilyProperties> queues(families);
        functions->vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, queues.data());
        for (uint32_t q = 0; q < families; ++q) {
            if (!queues[q].queueCount || !(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                || !instance.supportsPresent(devices[d], q, &surface)) continue;
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{};
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueFamilyIndex = q;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            const char *extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            VkDeviceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            info.queueCreateInfoCount = 1;
            info.pQueueCreateInfos = &queue;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &extension;
            VkDevice device = VK_NULL_HANDLE;
            if (functions->vkCreateDevice(devices[d], &info, nullptr, &device) != VK_SUCCESS) continue;
            instance.deviceFunctions(device)->vkDestroyDevice(device, nullptr);
            instance.resetDeviceFunctions(device);
            std::fputs("OMAAUDIO_VULKAN_READY\n", stdout);
            return 0;
        }
    }
    return 1;
}

bool runProbe(const QString &executable, const QStringList &arguments,
              const QProcessEnvironment &environment, QString &diagnostic, int timeoutMs)
{
    QProcess child;
    auto childEnvironment = environment;
    childEnvironment.insert(probeVariable, "1");
    childEnvironment.remove(restartVariable);
    childEnvironment.remove(recoveryVariable);
    child.setProcessEnvironment(childEnvironment);
    child.setProgram(executable);
    child.setArguments(arguments);
    child.setChildProcessModifier([] {
        const rlimit noCore{0, 0};
        (void)::setrlimit(RLIMIT_CORE, &noCore);
    });
    // Discard potentially unbounded driver diagnostics; the fixed stdout handshake
    // plus normal exit identifies success. Main app errors still remain visible.
    child.setStandardErrorFile(QProcess::nullDevice());
    QElapsedTimer deadline;
    deadline.start();
    child.start();
    if (!child.waitForStarted(timeoutMs)) {
        diagnostic = QStringLiteral("Graphics probe could not start: %1").arg(child.errorString());
        return false;
    }
    QByteArray output;
    while (child.state() != QProcess::NotRunning && deadline.elapsed() < timeoutMs) {
        child.waitForReadyRead(static_cast<int>(timeoutMs - deadline.elapsed()));
        output += child.readAllStandardOutput();
        if (output.size() > 4096) break;
    }
    output += child.readAllStandardOutput();
    if (child.state() != QProcess::NotRunning || output.size() > 4096) {
        child.kill();
        child.waitForFinished();
        diagnostic = QStringLiteral("Graphics probe exceeded its time/output limit; using software rendering.");
        return false;
    }
    const bool ok = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0
        && output == "OMAAUDIO_VULKAN_READY\n";
    diagnostic = ok ? QString() : QStringLiteral("Vulkan is unavailable; using software rendering.");
    return ok;
}

int restartInSoftware(const QString &executable, const QStringList &arguments,
                      const QString &checkpoint)
{
    qputenv(restartVariable, "1");
    qputenv(recoveryVariable, checkpoint.toUtf8());
    qputenv("QT_QUICK_BACKEND", "software");
    qunsetenv("QSG_RHI_BACKEND");
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(arguments.size()) + 1);
    storage.push_back(executable.toLocal8Bit());
    for (const auto &argument : arguments) storage.push_back(argument.toLocal8Bit());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(argv.front(), argv.data());
    std::fprintf(stderr, "OmaAudio could not restart: %s. Session checkpoint: %s\n",
                 std::strerror(errno), checkpoint.toLocal8Bit().constData());
    return 1;
}

GraphicsGuard::GraphicsGuard(QQuickWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
    window->installEventFilter(this);
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        m_presented.store(true);
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError, const QString &message) { report(message); },
            Qt::DirectConnection);
    connect(window, &QQuickWindow::sceneGraphInvalidated, this, [this] {
        if (m_exposed.load() && m_presented.load())
            report(QStringLiteral("The graphics context was lost."));
    }, Qt::DirectConnection);
}

void GraphicsGuard::disarm() { m_armed.store(false); }

void GraphicsGuard::report(const QString &message)
{
    if (!m_armed.load() || m_reported.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this, message] {
        if (m_armed.load()) emit failed(message);
    }, Qt::QueuedConnection);
}

bool GraphicsGuard::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_window) {
        if (event->type() == QEvent::Expose) m_exposed.store(m_window->isExposed());
        else if (event->type() == QEvent::Hide) {
            m_exposed.store(false);
            m_presented.store(false);
        }
    }
    return QObject::eventFilter(object, event);
}
} // namespace omaaudio::graphics
