#include "audiosetup.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QVariantList>

#include <algorithm>

namespace omaaudio::audiosetup {

namespace {

struct Node {
    int id = -1;
    QString name, description, mediaClass, clientApi, application, state;
    int channels = 0, rate = 0, deviceId = -1;
};

struct Port {
    int id = -1, nodeId = -1;
    QString name, direction;
};

QString str(const QJsonObject &props, const char *key) { return props.value(QLatin1String(key)).toString(); }
int num(const QJsonObject &props, const char *key, int fallback = -1)
{
    const QJsonValue v = props.value(QLatin1String(key));
    if (v.isDouble()) return v.toInt();
    if (v.isString()) { bool ok = false; const int n = v.toString().toInt(&ok); return ok ? n : fallback; }
    return fallback;
}

QVariantMap portMap(const Port &p) { return {{"id", p.id}, {"name", p.name}}; }

} // namespace

QString rateLabel(int rate)
{
    if (rate <= 0) return {};
    if (rate % 1000 == 0) return QStringLiteral("%1 kHz").arg(rate / 1000);
    return QStringLiteral("%1 kHz").arg(QString::number(rate / 1000.0, 'g', 4));
}

QVariantMap parse(const QJsonArray &pwDump, const QString &procCards, const QString &procPcm,
                  const QString &outputNode, const QString &captureNode)
{
    QVariantMap out;
    QVariantMap settings{{"clockRate", 0}, {"forceRate", 0}, {"quantum", 0}, {"allowedRates", QVariantList{}}};
    QVariantMap defaults{{"sink", QString()}, {"source", QString()}};
    QList<Node> nodes;
    QList<Port> ports;
    QList<QJsonObject> devices;
    struct Link { int outputNode, inputNode, outputPort, inputPort; };
    QList<Link> links;

    for (const QJsonValue &value : pwDump) {
        const QJsonObject object = value.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        const QJsonObject info = object.value(QStringLiteral("info")).toObject();
        const QJsonObject props = info.value(QStringLiteral("props")).toObject();
        if (type == QLatin1String("PipeWire:Interface:Metadata")) {
            for (const QJsonValue &entry : object.value(QStringLiteral("metadata")).toArray()) {
                const QJsonObject m = entry.toObject();
                const QString key = m.value(QStringLiteral("key")).toString();
                const QJsonValue v = m.value(QStringLiteral("value"));
                if (key == QLatin1String("clock.rate")) settings["clockRate"] = v.toVariant().toInt();
                else if (key == QLatin1String("clock.force-rate")) settings["forceRate"] = v.toVariant().toInt();
                else if (key == QLatin1String("clock.quantum")) settings["quantum"] = v.toVariant().toInt();
                else if (key == QLatin1String("clock.allowed-rates")) {
                    QVariantList rates;
                    if (v.isArray()) for (const QJsonValue &r : v.toArray()) rates.append(r.toVariant().toInt());
                    else {
                        static const QRegularExpression digits(QStringLiteral("\\d+"));
                        auto it = digits.globalMatch(v.toVariant().toString());
                        while (it.hasNext()) rates.append(it.next().captured(0).toInt());
                    }
                    settings["allowedRates"] = rates;
                } else if (key == QLatin1String("default.audio.sink")) defaults["sink"] = v.toObject().value(QStringLiteral("name")).toString();
                else if (key == QLatin1String("default.audio.source")) defaults["source"] = v.toObject().value(QStringLiteral("name")).toString();
            }
        } else if (type == QLatin1String("PipeWire:Interface:Node")) {
            Node n;
            n.id = object.value(QStringLiteral("id")).toInt(-1);
            n.name = str(props, "node.name");
            n.mediaClass = str(props, "media.class");
            n.clientApi = str(props, "client.api");
            n.application = str(props, "application.name");
            n.description = str(props, "node.description");
            if (n.description.isEmpty()) n.description = str(props, "node.nick");
            if (n.description.isEmpty()) n.description = n.application;
            if (n.description.isEmpty()) n.description = n.name;
            n.state = info.value(QStringLiteral("state")).toString();
            n.channels = num(props, "audio.channels", 0);
            n.rate = num(props, "audio.rate", 0);
            n.deviceId = num(props, "device.id", -1);
            const QJsonArray format = info.value(QStringLiteral("params")).toObject().value(QStringLiteral("Format")).toArray();
            if (!format.isEmpty()) {
                const QJsonObject f = format.first().toObject();
                if (f.contains(QStringLiteral("rate"))) n.rate = f.value(QStringLiteral("rate")).toInt(n.rate);
                if (f.contains(QStringLiteral("channels"))) n.channels = f.value(QStringLiteral("channels")).toInt(n.channels);
            }
            nodes.append(n);
        } else if (type == QLatin1String("PipeWire:Interface:Port")) {
            Port p;
            p.id = object.value(QStringLiteral("id")).toInt(-1);
            p.nodeId = num(props, "node.id", -1);
            p.name = str(props, "port.name");
            p.direction = info.value(QStringLiteral("direction")).toString();
            if (p.direction.isEmpty()) p.direction = str(props, "port.direction");
            ports.append(p);
        } else if (type == QLatin1String("PipeWire:Interface:Device")) {
            QJsonObject d = props;
            d.insert(QStringLiteral("__id"), object.value(QStringLiteral("id")).toInt(-1));
            devices.append(d);
        } else if (type == QLatin1String("PipeWire:Interface:Link")) {
            links.append({info.value(QStringLiteral("output-node-id")).toInt(-1),
                          info.value(QStringLiteral("input-node-id")).toInt(-1),
                          info.value(QStringLiteral("output-port-id")).toInt(-1),
                          info.value(QStringLiteral("input-port-id")).toInt(-1)});
        }
    }

    const auto nodeById = [&](int id) -> const Node * {
        for (const Node &n : nodes) if (n.id == id) return &n;
        return nullptr;
    };
    const auto nodeByName = [&](const QString &name) -> const Node * {
        for (const Node &n : nodes) if (n.name == name) return &n;
        return nullptr;
    };
    const auto portsOf = [&](int nodeId, const QString &direction) {
        QList<Port> found;
        for (const Port &p : ports) if (p.nodeId == nodeId && p.direction == direction) found.append(p);
        std::sort(found.begin(), found.end(), [](const Port &a, const Port &b) { return a.id < b.id; });
        return found;
    };

    // Devices, with the card index PipeWire's ALSA plugin reports.
    QVariantList deviceList;
    QHash<int, int> cardOfDevice;   // device id -> alsa card index
    for (const QJsonObject &d : devices) {
        QVariantMap m;
        const int id = d.value(QStringLiteral("__id")).toInt(-1);
        m["id"] = id;
        m["name"] = str(d, "device.name");
        m["description"] = str(d, "device.description");
        m["nick"] = str(d, "device.nick");
        m["api"] = str(d, "device.api");
        m["cardIndex"] = num(d, "api.alsa.card", -1);
        m["alsaPath"] = str(d, "api.alsa.path");
        QVariantList names;
        for (const Node &n : nodes) if (n.deviceId == id) names.append(n.name);
        m["nodeNames"] = names;
        cardOfDevice[id] = m["cardIndex"].toInt();
        deviceList.append(m);
    }

    // Sinks and sources: the usable endpoints.
    const auto endpoint = [&](const Node &n) {
        QVariantMap m;
        m["id"] = n.id;
        m["name"] = n.name;
        m["description"] = n.description;
        m["state"] = n.state;
        m["channels"] = n.channels;
        m["rate"] = n.rate;
        m["deviceId"] = n.deviceId;
        m["cardIndex"] = cardOfDevice.value(n.deviceId, -1);
        return m;
    };
    QVariantList sinks, sources;
    QSet<int> usableCards;
    for (const Node &n : nodes) {
        if (n.mediaClass == QLatin1String("Audio/Sink") || n.mediaClass == QLatin1String("Audio/Duplex")) {
            QVariantMap m = endpoint(n);
            m["isDefault"] = n.name == defaults["sink"].toString();
            sinks.append(m);
            if (m["cardIndex"].toInt() >= 0) usableCards.insert(m["cardIndex"].toInt());
        }
        if (n.mediaClass == QLatin1String("Audio/Source") || n.mediaClass == QLatin1String("Audio/Duplex")) {
            QVariantMap m = endpoint(n);
            m["isDefault"] = n.name == defaults["source"].toString();
            sources.append(m);
            if (m["cardIndex"].toInt() >= 0) usableCards.insert(m["cardIndex"].toInt());
        }
    }

    // Our own streams and what they are wired to.
    const auto stream = [&](const Node *n, bool output) {
        QVariantMap m;
        m["present"] = n != nullptr;
        m["state"] = n ? n->state : QString();
        m["rate"] = n ? n->rate : 0;
        m["channels"] = n ? n->channels : 0;
        QVariantList peers, peerIds;
        if (n) {
            QSet<int> seen;
            for (const Link &l : links) {
                const int other = output ? (l.outputNode == n->id ? l.inputNode : -1)
                                         : (l.inputNode == n->id ? l.outputNode : -1);
                if (other < 0 || seen.contains(other)) continue;
                seen.insert(other);
                const Node *peer = nodeById(other);
                peers.append(peer ? peer->description : QStringLiteral("node %1").arg(other));
                peerIds.append(other);
            }
        }
        m[output ? "connectedTo" : "connectedFrom"] = peers;
        m[output ? "connectedToIds" : "connectedFromIds"] = peerIds;
        return m;
    };
    const Node *ours = nodeByName(outputNode);
    const Node *capture = nodeByName(captureNode);
    out["output"] = stream(ours, true);
    out["capture"] = stream(capture, false);
    QVariantList ourPorts, ourInputs;
    if (ours) for (const Port &p : portsOf(ours->id, QStringLiteral("output"))) ourPorts.append(portMap(p));
    if (capture) for (const Port &p : portsOf(capture->id, QStringLiteral("input"))) ourInputs.append(portMap(p));
    out["ourOutputPorts"] = ourPorts;
    out["ourCapturePorts"] = ourInputs;

    // JACK clients: nodes the pipewire-jack shim created.
    QVariantList jackClients;
    for (const Node &n : nodes) {
        if (n.clientApi != QLatin1String("jack")) continue;
        QVariantMap m;
        m["id"] = n.id;
        m["name"] = n.name;
        m["application"] = n.application.isEmpty() ? n.description : n.application;
        m["state"] = n.state;
        QVariantList inputs, outputs;
        for (const Port &p : portsOf(n.id, QStringLiteral("input"))) inputs.append(portMap(p));
        for (const Port &p : portsOf(n.id, QStringLiteral("output"))) outputs.append(portMap(p));
        m["inputPorts"] = inputs;
        m["outputPorts"] = outputs;
        m["direction"] = !inputs.isEmpty() && !outputs.isEmpty() ? QStringLiteral("duplex")
                       : !inputs.isEmpty() ? QStringLiteral("input") : QStringLiteral("output");
        bool toOurOutput = false, toOurCapture = false;
        for (const Link &l : links) {
            if (ours && l.outputNode == ours->id && l.inputNode == n.id) toOurOutput = true;
            if (capture && l.inputNode == capture->id && l.outputNode == n.id) toOurCapture = true;
        }
        m["connectedToOurOutput"] = toOurOutput;
        m["connectedToOurCapture"] = toOurCapture;
        jackClients.append(m);
    }

    // ALSA cards from the kernel, mapped to PipeWire's devices.
    QVariantList cards;
    {
        static const QRegularExpression cardLine(QStringLiteral("^\\s*(\\d+)\\s+\\[(\\S+)\\s*\\]:\\s*(.*)$"));
        static const QRegularExpression pcmLine(QStringLiteral("^(\\d+)-(\\d+):\\s*([^:]*?)\\s*:\\s*([^:]*?)\\s*:(.*)$"));
        QHash<int, QVariantList> pcms;
        for (const QString &line : procPcm.split(QLatin1Char('\n'))) {
            const auto m = pcmLine.match(line);
            if (!m.hasMatch()) continue;
            QVariantMap pcm;
            pcm["device"] = m.captured(2).toInt();
            pcm["name"] = m.captured(3).trimmed();
            const QString caps = m.captured(5);
            pcm["playback"] = caps.contains(QLatin1String("playback"));
            pcm["capture"] = caps.contains(QLatin1String("capture"));
            pcms[m.captured(1).toInt()].append(pcm);
        }
        const QStringList lines = procCards.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const auto m = cardLine.match(lines.at(i));
            if (!m.hasMatch()) continue;
            QVariantMap card;
            const int index = m.captured(1).toInt();
            card["index"] = index;
            card["id"] = m.captured(2);
            card["name"] = m.captured(3).trimmed();
            card["longName"] = i + 1 < lines.size() ? lines.at(i + 1).trimmed() : QString();
            card["pcms"] = pcms.value(index);
            card["deviceId"] = -1;
            card["deviceName"] = QString();
            for (const QVariant &d : deviceList) {
                const QVariantMap dm = d.toMap();
                if (dm["cardIndex"].toInt() == index) { card["deviceId"] = dm["id"]; card["deviceName"] = dm["name"]; break; }
            }
            card["usable"] = usableCards.contains(index);
            cards.append(card);
        }
    }

    out["settings"] = settings;
    out["defaults"] = defaults;
    out["sinks"] = sinks;
    out["sources"] = sources;
    out["devices"] = deviceList;
    out["alsaCards"] = cards;
    out["jackClients"] = jackClients;
    out["nodeCount"] = nodes.size();
    return out;
}

} // namespace omaaudio::audiosetup
