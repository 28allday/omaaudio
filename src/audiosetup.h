#pragma once

#include <QJsonArray>
#include <QString>
#include <QVariantMap>

// A read-only picture of the audio system, from what PipeWire and ALSA report,
// for the Audio Setup panel. Pure functions over text so the tests can hand
// them captured fixtures. Nothing here changes a default, opens a device or
// starts monitoring.
//
//   pwDump       the JSON array `pw-dump` prints
//   procCards    /proc/asound/cards
//   procPcm      /proc/asound/pcm
//   outputNode   the node name of OmaAudio's playback stream
//   captureNode  the node name of OmaAudio's capture stream
//
// The result is a QVariantMap for QML with these keys:
//   settings   {clockRate, forceRate, quantum, allowedRates: [int]}
//   defaults   {sink, source}                 node names, "" when unknown
//   output     {present, state, rate, channels, connectedTo: [description], connectedToIds: [int]}
//   capture    {present, state, rate, channels, connectedFrom: [description]}
//   sinks      [{id, name, description, state, channels, rate, isDefault, cardIndex, deviceId}]
//   sources    [same]
//   devices    [{id, name, description, nick, api, cardIndex, alsaPath, nodeNames: [name]}]
//   alsaCards  [{index, id, name, longName, pcms: [{device, name, playback, capture}],
//               deviceId, deviceName, usable}]      usable: a PipeWire sink or source exists for it
//   jackClients [{id, name, application, direction, state, inputPorts: [{id, name}],
//                outputPorts: [{id, name}], connectedToOurOutput, connectedToOurCapture}]
//   ourOutputPorts [{id, name}]                 OmaAudio's playback ports, for linking
//   ourCapturePorts [{id, name}]                OmaAudio's recording ports while a take runs
//   nodeCount
namespace omaaudio::audiosetup {

QVariantMap parse(const QJsonArray &pwDump, const QString &procCards, const QString &procPcm,
                  const QString &outputNode = QStringLiteral("omaaudio"),
                  const QString &captureNode = QStringLiteral("omaaudio-capture"));

// "44.1 kHz" for 44100, "48 kHz" for 48000: the way the interface writes rates.
QString rateLabel(int rate);

} // namespace omaaudio::audiosetup
