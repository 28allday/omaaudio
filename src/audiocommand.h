#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QStringList>
#include <QList>
#include <QPair>
#include <functional>
#include <utility>

// GUI-thread subprocesses with bounded output and a deadline. No nested event
// loop or blocking wait during commands: controls and Stop remain usable.
// Completion is always asynchronous, so the caller can first store its job handle.
class AudioCommand final : public QObject {
public:
    struct Result {
        QByteArray output;
        QString error;
        bool ok = false;
    };
    using Done = std::function<void(Result)>;

    AudioCommand(QObject *owner, QString program, QStringList arguments,
                 int timeoutMs, qsizetype outputLimit, Done done)
        : QObject(owner), m_limit(outputLimit), m_done(std::move(done))
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            m_failure = tr("The audio system did not reply in time. Refresh and try again.");
            m_process.kill();
        });
        connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { drain(); });
        connect(&m_process, &QProcess::readyReadStandardError, this, [this] { drain(); });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            drain();
            finish(code == 0 && status == QProcess::NormalExit);
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                m_failure = tr("%1 could not start. Check that the PipeWire tools are installed.").arg(m_process.program());
                finish(false);
            }
        });
        QTimer::singleShot(0, this, [this, program = std::move(program), arguments = std::move(arguments), timeoutMs] {
            if (m_cancelled) return;
            m_timer.start(timeoutMs);
            m_process.start(program, arguments);
        });
    }

    ~AudioCommand() override
    {
        m_process.disconnect(this);
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            // Owner teardown only; ordinary Stop/Cancel waits asynchronously.
            m_process.waitForFinished(1000);
        }
    }
    void cancel()
    {
        m_cancelled = true;
        m_done = {};
        m_timer.stop();
        if (m_process.state() == QProcess::NotRunning) deleteLater();
        else m_process.kill(); // finished() disposes of the job after the child exits
    }

private:
    void append(QByteArray &to, const QByteArray &bytes, qsizetype limit)
    {
        if (bytes.size() > limit - to.size()) {
            to.append(bytes.constData(), limit - to.size());
            m_failure = tr("The audio system returned too much information. Refresh and try again.");
            m_process.kill();
        } else to.append(bytes);
    }
    void drain()
    {
        append(m_output, m_process.readAllStandardOutput(), m_limit);
        append(m_error, m_process.readAllStandardError(), 64 * 1024);
    }
    void finish(bool ok)
    {
        if (m_finished) return;
        m_finished = true;
        m_timer.stop();
        QString error = m_failure;
        if (!ok && error.isEmpty()) {
            error = QString::fromUtf8(m_error).trimmed();
            if (error.isEmpty()) error = tr("The audio command failed. Refresh and try again.");
        }
        auto done = std::move(m_done);
        deleteLater();
        if (done) done({std::move(m_output), error, ok && error.isEmpty()});
    }
    QProcess m_process;
    QTimer m_timer;
    qsizetype m_limit;
    QByteArray m_output, m_error;
    QString m_failure;
    Done m_done;
    bool m_cancelled = false, m_finished = false;
};

// A small, ordered port sequence. Cancellation never undoes pre-existing links;
// callers refresh the graph and report any channels already changed.
class AudioLinks final : public QObject {
public:
    using Ports = QList<QPair<int, int>>;
    using Done = std::function<void(QString, int)>;
    AudioLinks(QObject *owner, Ports ports, bool disconnectPorts, Done done)
        : QObject(owner), m_ports(std::move(ports)), m_disconnect(disconnectPorts), m_done(std::move(done))
    {
        QTimer::singleShot(0, this, [this] { next(); });
    }
    ~AudioLinks() override { if (m_command) m_command->cancel(); }
    void cancel()
    {
        m_cancelled = true;
        m_done = {};
        if (m_command) { m_command->cancel(); m_command = nullptr; }
        // A killed QProcess must finish before its owning object is destroyed.
        // Keep cancelled commands under the backend until their deferred deletion.
        deleteLater();
    }
    int completed() const { return m_index; }
private:
    void next()
    {
        if (m_cancelled) return;
        if (m_index == m_ports.size()) { finish({}); return; }
        const auto pair = m_ports[m_index];
        QStringList arguments;
        if (m_disconnect) arguments << QStringLiteral("-d");
        arguments << QString::number(pair.first) << QString::number(pair.second);
        // The backend owns each command independently of this short-lived sequence.
        m_command = new AudioCommand(parent(), QStringLiteral("pw-link"), arguments, 3000, 64 * 1024,
            [this](AudioCommand::Result result) {
                m_command = nullptr;
                // pw-link can report an already existing connection; it is success
                // only for connect, never for timeout/output-limit/startup errors.
                if (!result.ok && !m_disconnect && result.error.contains(QStringLiteral("File exists")))
                    result.ok = true;
                if (!result.ok) { finish(result.error); return; }
                ++m_index;
                next();
            });
    }
    void finish(QString error)
    {
        auto done = std::move(m_done);
        deleteLater();
        if (done) done(std::move(error), m_index);
    }
    Ports m_ports;
    bool m_disconnect, m_cancelled = false;
    int m_index = 0;
    AudioCommand *m_command = nullptr;
    Done m_done;
};
