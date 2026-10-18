#include "aafimport.h"
#include "storagepaths.h"
#include "io/audio_file.h"
#include "io/resampler.h"
#include "video/reference.h"
#include "session/uuid.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <map>
#include <limits>
#include <utility>

namespace omaaudio::aaf {
using namespace omawave;
namespace {
constexpr qint64 kMaxFrames = 1000000000000LL;
bool cancelled(const std::atomic<bool>* flag) { return flag && flag->load(std::memory_order_relaxed); }
void checkCancel(const std::atomic<bool>* flag) {
    if (cancelled(flag)) throw std::runtime_error("AAF import cancelled; the session is unchanged.");
}
void require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
qint64 integer(const QJsonObject& o, const char* key, qint64 minimum, qint64 maximum) {
    const auto v = o.value(QLatin1String(key));
    const double n = v.toDouble(std::numeric_limits<double>::quiet_NaN());
    require(v.isDouble() && std::isfinite(n) && std::floor(n)==n && n>=minimum && n<=maximum,
            "The AAF reader returned an invalid numeric field.");
    return static_cast<qint64>(n);
}
QString helperPath(const QString& script) {
    const auto binary = QCoreApplication::applicationDirPath();
    for (const auto& path : {binary+"/../share/omaaudio/import/"+script,
                            binary+"/../tools/"+script, binary+"/../../tools/"+script,
                            binary+"/../../../tools/"+script})
        if (QFileInfo(path).isFile()) return QFileInfo(path).canonicalFilePath();
    return {};
}
QJsonObject parse(const QString& path, const QString& mediaFolder, const QString& scratch,
                  std::uint32_t sampleRate, const std::atomic<bool>* cancel) {
    checkCancel(cancel);
    const auto python = QStandardPaths::findExecutable("python3");
    const auto helper = helperPath(formatOf(path) == QLatin1String("OMF") ? QStringLiteral("omf_import.py")
                                                                         : QStringLiteral("aaf_import.py"));
    require(!python.isEmpty() && !helper.isEmpty(), "AAF import files are missing. Reinstall the OmaAudio package with its Python runtime.");
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QStringList args{"-I","-B",helper,path,scratch,"--sample-rate",QString::number(sampleRate)};
    if (!mediaFolder.isEmpty()) args << "--media-folder" << mediaFolder;
    process.start(python,args,QIODevice::ReadOnly);
    require(process.waitForStarted(5000), "The AAF reader could not start.");
    QByteArray output, errors;
    QElapsedTimer timer; timer.start();
    while (process.state()!=QProcess::NotRunning) {
        process.waitForFinished(30);
        output += process.readAllStandardOutput(); errors += process.readAllStandardError();
        if (cancelled(cancel) || timer.elapsed()>180000 || output.size()>16*1024*1024 || errors.size()>1024*1024) {
            process.kill(); process.waitForFinished(5000);
            checkCancel(cancel);
            throw std::runtime_error("The AAF reader exceeded its time or output limit; the session is unchanged.");
        }
    }
    output += process.readAllStandardOutput(); errors += process.readAllStandardError();
    checkCancel(cancel);
    require(output.size()<=16*1024*1024 && errors.size()<=1024*1024, "The AAF reader output is too large.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output,&error);
    require(error.error==QJsonParseError::NoError && document.isObject(), "The AAF reader could not read this file. Export a new AAF with WAV audio from Resolve.");
    const auto object = document.object();
    if (!object.value("error").toString().isEmpty()) throw std::runtime_error(object.value("error").toString().toStdString());
    require(process.exitStatus()==QProcess::NormalExit && process.exitCode()==0,
            "The AAF reader stopped unexpectedly; the session is unchanged.");
    require(integer(object,"version",1,1)==1 && integer(object,"sample_rate",sampleRate,sampleRate)==sampleRate,
            "The AAF import description has an unsupported version or rate.");
    return object;
}
session::AudioBuffer selectChannel(const session::AudioBuffer& source, std::uint32_t channel,
                                   const std::atomic<bool>* cancel) {
    auto backing=session::DiskAudio::create(1,source.frames());
    std::vector<float> block(16384);
    for (std::size_t at=0; at<source.frames(); at+=block.size()) {
        checkCancel(cancel);
        const auto count=std::min(block.size(),source.frames()-at);
        auto view=std::span<float>(block.data(),count);
        if (source.diskStorage()) require(source.diskStorage()->readPlane(channel,at,view),"Could not read an AAF source channel.");
        else std::copy_n(source.plane(channel).data()+at,count,view.data());
        backing->writePlane(0,at,view);
    }
    backing->seal();
    return session::AudioBuffer::fromDisk(std::move(backing));
}
}

namespace {
// One FFmpeg run to its end, cancellable; on failure the last thing it said.
bool runFfmpeg(const QString& ffmpeg, const QStringList& args, const std::atomic<bool>* cancel,
               qint64 timeoutMs, QString& error) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg, args, QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) { error = QStringLiteral("FFmpeg could not start."); return false; }
    QByteArray said; QElapsedTimer timer; timer.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        said += process.readAll();
        if (said.size() > 65536) said = said.right(16384);
        if (cancelled(cancel) || timer.elapsed() > timeoutMs) {
            process.kill(); process.waitForFinished(5000);
            checkCancel(cancel);
            error = QStringLiteral("FFmpeg took too long."); return false;
        }
    }
    said += process.readAll();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) return true;
    const auto lines = QString::fromUtf8(said).trimmed().split(QLatin1Char('\n'));
    error = lines.isEmpty() || lines.last().isEmpty() ? QStringLiteral("FFmpeg stopped with an error.") : lines.last().left(300);
    return false;
}

struct Picture { QString path; double start = 0; int shots = 0; };

// An edited picture track built into one reference video at the timeline's
// rate: each shot cut from its file, black between shots and for a shot whose
// file is missing. Every piece is a whole number of frames, so the joins stay
// on the frame, and the pieces are joined without encoding them again. Small
// (540 lines) and with a keyframe every half second, so it scrubs quickly.
Picture buildPicture(const QJsonObject& video, const std::atomic<bool>* cancel, QString& error) {
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const auto ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) { error = QStringLiteral("FFmpeg is not installed."); return {}; }
    const auto rateText = video.value("rate").toString();
    const auto parts = rateText.split(QLatin1Char('/'));
    const double rate = parts.size() == 2 ? parts[0].toDouble() / std::max(1e-9, parts[1].toDouble()) : rateText.toDouble();
    if (!std::isfinite(rate) || rate < 1 || rate > 240) { error = QStringLiteral("The picture's frame rate is not supported."); return {}; }
    struct Shot { qint64 from, to; double offset; QString path; };
    std::vector<Shot> shots;
    QString first;
    for (const auto& value : video.value("shots").toArray()) {
        const auto o = value.toObject();
        const double time = o.value("time").toDouble(), offset = o.value("offset").toDouble(), length = o.value("duration").toDouble();
        if (!std::isfinite(time) || !std::isfinite(offset) || !std::isfinite(length) || length <= 0 || offset < 0) continue;
        const auto path = o.value("path").toString();
        const bool usable = !path.isEmpty() && QFileInfo(path).isAbsolute() && QFileInfo(path).isFile();
        shots.push_back({std::llround(time * rate), std::llround((time + length) * rate), offset, usable ? path : QString()});
        if (first.isEmpty() && usable) first = path;
    }
    std::stable_sort(shots.begin(), shots.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
    if (shots.empty() || first.isEmpty()) { error = QStringLiteral("none of its video files were found."); return {}; }
    // The reference's size: the first shot's shape, 540 lines high (or less).
    QProcess probe;
    probe.start(ffprobe, {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", first});
    probe.waitForFinished(30000);
    const auto size = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().split(QLatin1Char('x'));
    const double width = size.size() == 2 ? size[0].toDouble() : 16, height = size.size() == 2 ? size[1].toDouble() : 9;
    const int h = height >= 2 ? std::min(540, int(height) / 2 * 2) : 540;
    const int w = std::max(2, int(std::lround(h * (width > 0 && height > 0 ? width / height : 16.0 / 9.0) / 2)) * 2);
    QProcess encoders;
    encoders.start(ffmpeg, {"-hide_banner", "-encoders"});
    encoders.waitForFinished(15000);
    const QStringList codec = encoders.readAllStandardOutput().contains("libx264")
        ? QStringList{"-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-bf", "0"}
        : QStringList{"-c:v", "mpeg4", "-q:v", "4", "-bf", "0"};
    const auto r = QString::number(rate, 'g', 12);
    const QStringList common{"-nostdin", "-y", "-hide_banner", "-loglevel", "error"};
    const QStringList output = codec + QStringList{"-g", QString::number(std::max(1, int(std::lround(rate / 2)))),
        "-pix_fmt", "yuv420p", "-r", r, "-an", "-f", "mp4"};
    const auto cache = storage::locations().cacheDir;
    QTemporaryDir scratch(cache + "/aaf-picture-XXXXXX");
    if (!QDir().mkpath(cache) || !scratch.isValid()) { error = QStringLiteral("its temporary folder could not be made."); return {}; }
    QStringList pieces;
    qint64 cursor = shots.front().from;
    const auto piece = [&](const QStringList& input, const QString& filter, qint64 frames) {
        const auto file = scratch.filePath(QStringLiteral("piece-%1.mp4").arg(pieces.size(), 5, 10, QLatin1Char('0')));
        const qint64 timeout = std::max<qint64>(120000, qint64(frames / rate * 4000));
        if (!runFfmpeg(ffmpeg, common + input + QStringList{"-vf", filter, "-frames:v", QString::number(frames)} + output + QStringList{file},
                       cancel, timeout, error)) return false;
        pieces << file; return true;
    };
    const auto black = QStringLiteral("color=c=black:s=%1x%2:r=%3").arg(w).arg(h).arg(r);
    for (auto shot : shots) {
        checkCancel(cancel);
        if (shot.from > cursor && !piece({"-f", "lavfi", "-i", black}, QStringLiteral("format=yuv420p"), shot.from - cursor)) return {};
        if (shot.from < cursor) { shot.offset += double(cursor - shot.from) / rate; shot.from = cursor; }
        if (shot.to <= shot.from) continue;
        const bool ok = shot.path.isEmpty()
            ? piece({"-f", "lavfi", "-i", black}, QStringLiteral("format=yuv420p"), shot.to - shot.from)
            : piece({"-ss", QString::number(shot.offset, 'f', 6), "-i", shot.path},
                    QStringLiteral("scale=%1:%2:force_original_aspect_ratio=decrease,pad=%1:%2:(ow-iw)/2:(oh-ih)/2,setsar=1,"
                                   "fps=%3,format=yuv420p,tpad=stop=-1:stop_mode=clone").arg(w).arg(h).arg(r),
                    shot.to - shot.from);
        if (!ok) { error = QFileInfo(shot.path).fileName() + QStringLiteral(": ") + error; return {}; }
        cursor = shot.to;
    }
    QFile list(scratch.filePath(QStringLiteral("pieces.txt")));
    if (!list.open(QIODevice::WriteOnly)) { error = QStringLiteral("its piece list could not be written."); return {}; }
    for (const auto& file : pieces) list.write(("file '" + QString(file).replace("'", "'\\''") + "'\n").toUtf8());
    list.close();
    const auto folder = storage::locations().stateDir + QStringLiteral("/pictures");
    if (!QDir().mkpath(folder)) { error = QStringLiteral("its folder could not be made."); return {}; }
    const auto target = folder + QStringLiteral("/") + QString::fromStdString(session::Uuid::generate().toString()) + QStringLiteral(".mp4");
    if (!runFfmpeg(ffmpeg, common + QStringList{"-f", "concat", "-safe", "0", "-i", list.fileName(), "-c", "copy",
                                                "-movflags", "+faststart", target}, cancel, 600000, error)) {
        QFile::remove(target); return {};
    }
    return {target, double(shots.front().from) / rate, int(shots.size())};
}
}

QString formatOf(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("omf") || suffix == QLatin1String("omfi")) return QStringLiteral("OMF");
    if (suffix == QLatin1String("aaf")) return QStringLiteral("AAF");
    QFile file(path);
    if (file.size() >= 24 && file.open(QIODevice::ReadOnly) && file.seek(file.size() - 24)
        && file.read(8) == QByteArray("\xa4" "CM" "\xa5" "Hdr" "\xd7", 8)) return QStringLiteral("OMF");
    return QStringLiteral("AAF");
}

Result read(const QString& path, const QString& mediaFolder, session::Project candidate,
            const std::atomic<bool>* cancel) {
    Result result;
    result.format = formatOf(path);
    try {
        require(QFileInfo(path).isFile(),"The selected AAF file is missing.");
        const auto cache=storage::locations().cacheDir;
        require(QDir().mkpath(cache),"The AAF temporary folder could not be created.");
        QTemporaryDir scratch(cache+"/aaf-import-XXXXXX");
        require(scratch.isValid(),"The AAF temporary folder could not be created.");
        const auto object=parse(path,mediaFolder,scratch.path(),candidate.sampleRate(),cancel);
        const auto sources=object.value("sources").toArray(), tracks=object.value("tracks").toArray();
        require(!sources.isEmpty() && sources.size()<=10000 && !tracks.isEmpty() && tracks.size()<=256,
                "The AAF has no usable audio, or contains too many tracks or sources.");
        for (const auto& warning : object.value("warnings").toArray()) {
            require(warning.isString() && warning.toString().size()<=4096 && result.warnings.size()<200,"Invalid AAF import warning.");
            result.warnings << warning.toString();
        }
        std::vector<session::SourceId> originalSources;
        for (const auto& source : candidate.sources()) if (source) originalSources.push_back(source->id);
        std::vector<session::SourceId> sourceIds;
        std::map<std::pair<QString,qint64>, session::SourceId> decoded;
        std::map<std::vector<const session::DiskAudio*>, session::SourceId> combined;
        const auto namedSource = [&](session::SourceId existing, const QString& name, const QString& original) {
            const auto* source=candidate.source(existing);
            if (source->name==name.toStdString()) return existing;
            // All imported buffers are sealed disk storage. Aliases preserve
            // AAF clip/source names without duplicating the actual sample data.
            return candidate.addSource(name.toStdString(),original.toStdString(),source->audio,
                source->originalSampleRate,{},std::nullopt,0,source->provenance);
        };
        for (const auto& item : sources) {
            checkCancel(cancel);
            require(item.isObject(),"Invalid AAF source description.");
            const auto source=item.toObject();
            if (source.contains("channels")) {
                const auto channels=source.value("channels").toArray();
                require(!channels.isEmpty() && channels.size()<=32,"Invalid AAF channel combination.");
                std::vector<session::SourceId> ids;
                for (const auto& channel : channels) {
                    QJsonObject reference{{"id",channel}};
                    require(!sourceIds.empty(),"Invalid AAF channel source order.");
                    ids.push_back(sourceIds[integer(reference,"id",0,sourceIds.size()-1)]);
                }
                std::vector<const session::DiskAudio*> buffers;
                for (const auto id : ids) buffers.push_back(candidate.source(id)->audio.diskStorage().get());
                if (const auto known=combined.find(buffers); known!=combined.end()) {
                    sourceIds.push_back(namedSource(known->second,source.value("name").toString(),path)); continue;
                }
                const auto frames=candidate.source(ids.front())->audio.frames();
                auto backing=session::DiskAudio::create(ids.size(),frames);
                std::vector<float> block(16384);
                for (std::size_t channel=0; channel<ids.size(); ++channel) {
                    const auto& audio=candidate.source(ids[channel])->audio;
                    require(audio.channels()==1 && audio.frames()==frames,"AAF channels have incompatible source lengths or layouts.");
                    for (std::size_t at=0; at<frames; at+=block.size()) {
                        checkCancel(cancel);
                        auto view=std::span<float>(block.data(),std::min(block.size(),frames-at));
                        require(audio.diskStorage()->readPlane(0,at,view),"Could not read a combined AAF channel.");
                        backing->writePlane(channel,at,view);
                    }
                }
                backing->seal();
                const auto id=candidate.addSource(source.value("name").toString().toStdString(),path.toStdString(),session::AudioBuffer::fromDisk(backing));
                sourceIds.push_back(id); combined.emplace(std::move(buffers),id); continue;
            }
            const auto filename=source.value("path").toString();
            require(QFileInfo(filename).isAbsolute() && QFileInfo(filename).isFile(),"An AAF audio source is missing.");
            const auto channel=integer(source,"channel",-1,31);
            const auto key=std::make_pair(QFileInfo(filename).canonicalFilePath(),channel);
            if (const auto known=decoded.find(key); known!=decoded.end()) {
                sourceIds.push_back(namedSource(known->second,source.value("name").toString(),source.value("original").toString())); continue;
            }
            auto file=io::readAudioFile(filename.toStdString(),cancel,{});
            if (!file.ok) throw std::runtime_error((QFileInfo(filename).fileName()+": "+QString::fromStdString(file.error)).toStdString());
            require(file.audio.channels()>0 && file.audio.channels()<=32 && file.audio.frames()>0,"An AAF source has no supported audio channels.");
            if (!file.error.empty()) result.warnings << QString::fromStdString(file.error);
            if (channel>=0) {
                require(channel<file.audio.channels(),"An AAF channel number exceeds the source's channel count.");
                file.audio=selectChannel(std::as_const(file.audio),channel,cancel);
            }
            const auto originalRate=file.sampleRate;
            if (originalRate!=candidate.sampleRate()) {
                file.audio=io::Resampler(originalRate,candidate.sampleRate()).process(file.audio,cancel);
                for (auto& marker : file.metadata.markers) {
                    const auto end=session::rescaleFrames(marker.position+marker.length,originalRate,candidate.sampleRate());
                    marker.position=session::rescaleFrames(marker.position,originalRate,candidate.sampleRate());
                    marker.length=std::max<std::int64_t>(0,end-marker.position);
                }
                if (file.metadata.bext.present) file.metadata.bext.timeReference=session::rescaleFrames(file.metadata.bext.timeReference,originalRate,candidate.sampleRate());
            }
            checkCancel(cancel);
            // A session may contain thousands of short clips. Keep their total
            // audio memory bounded, not just the memory of each individual file.
            file.audio.storeOnDisk(0,cancel);
            sourceIds.push_back(candidate.addSource(source.value("name").toString().toStdString(),
                source.value("original").toString().toStdString(),std::move(file.audio),
                originalRate==candidate.sampleRate()?std::nullopt:std::optional<std::uint32_t>(originalRate),
                {},std::nullopt,0,std::move(file.metadata)));
            decoded.emplace(key,sourceIds.back());
        }
        const bool empty=candidate.tracks().empty();
        for (const auto& item : tracks) {
            checkCancel(cancel);
            require(item.isObject(),"Invalid AAF track description.");
            const auto track=item.toObject();
            const auto name=track.value("name").toString();
            require(name.size()<=4096 && track.value("clips").isArray(),"Invalid AAF track name or clips.");
            auto id=candidate.addTrack(name.toStdString());
            auto* target=candidate.track(id);
            ++result.tracks;
            for (const auto& value : track.value("clips").toArray()) {
                require(value.isObject() && ++result.clips<=100000,"The AAF has invalid clips or too many clips.");
                const auto entry=value.toObject();
                session::Clip clip;
                clip.source=sourceIds[integer(entry,"source",0,sourceIds.size()-1)];
                clip.timelineStart=integer(entry,"start",0,kMaxFrames);
                clip.sourceOffset=integer(entry,"offset",0,kMaxFrames);
                clip.length=integer(entry,"length",1,kMaxFrames-clip.timelineStart);
                clip.fadeIn=integer(entry,"fade_in",0,kMaxFrames);
                clip.fadeOut=integer(entry,"fade_out",0,kMaxFrames);
                clip.fadeInOffset=integer(entry,"fade_in_offset",0,kMaxFrames);
                clip.fadeOutOffset=integer(entry,"fade_out_offset",0,kMaxFrames);
                const double gain=entry.value("gain").toDouble(-1);
                require(std::isfinite(gain) && gain>=0 && gain<=64,"An AAF clip gain is invalid.");clip.gain=gain;
                if (entry.contains("envelope")) {
                    // Clip volume keyframes: clip-local frames, linear gain (Oma's 0..4).
                    const auto points=entry.value("envelope").toArray();
                    require(points.size()>=1 && points.size()<=qsizetype(session::AutomationCurve::kMaxPoints),"An AAF volume envelope is invalid.");
                    session::FramePos previous=-1;
                    for (const auto& point : points) {
                        const auto pair=point.toArray();
                        require(pair.size()==2,"An AAF volume envelope is invalid.");
                        const double frame=pair[0].toDouble(-1), level=pair[1].toDouble(-1);
                        require(std::isfinite(frame) && frame==std::floor(frame) && frame>previous && frame<=double(clip.length)
                                && std::isfinite(level) && level>=0 && level<=4,"An AAF volume envelope is invalid.");
                        session::AutomationPoint keyframe; keyframe.frame=session::FramePos(frame); keyframe.value=level;
                        clip.volumeEnvelope.points.push_back(keyframe);
                        previous=session::FramePos(frame);
                    }
                    clip.volumeEnvelope.enabled=true;
                }
                require(clip.validSource(candidate.source(clip.source)->audio.frames()),
                        "An AAF clip extends beyond its source audio. Export the AAF with its audio handles, then import again.");
                require(clip.hasValidFades(),"An AAF fade is outside its source clip.");
                target->clips.push_back(clip);
            }
            std::stable_sort(target->clips.begin(),target->clips.end(),[](const auto& a,const auto& b){return a.timelineStart<b.timelineStart;});
        }
        require(result.clips>0,"The AAF has no usable audio clips.");
        if (!object.value("time_reference").isNull()) {
            const auto reference=integer(object,"time_reference",0,kMaxFrames);
            if (empty && !candidate.metadata().timeReferenceSet) {
                candidate.metadata().timeReferenceSet=true; candidate.metadata().timeReference=reference;
            } else if (!candidate.metadata().timeReferenceSet || candidate.metadata().timeReference!=reference) {
                result.warnings << "The AAF start timecode differs from this session. Clips keep their positions relative to timeline start; the existing session timecode was kept.";
            }
        }
        // The timeline's picture becomes the reference video, placed where the
        // timeline shows it, in the same Undo step as the audio. A session that
        // already has a video keeps it.
        if (const auto video = object.value("video"); video.isObject() && video.toObject().contains("shots")) {
            // An edited picture: built into one video, kept with the recordings
            // until the project is saved, then saved inside it.
            const auto entry = video.toObject();
            const auto name = entry.value("name").toString().left(256);
            QString why;
            if (!candidate.video().empty())
                result.warnings << QStringLiteral("This session already has a video, so it was kept; the AAF's picture was not attached.");
            else if (const auto reason = video::videoSupportReason(); !reason.empty())
                result.warnings << QStringLiteral("The AAF's picture was not attached: %1").arg(QString::fromStdString(reason));
            else if (const auto built = buildPicture(entry, cancel, why); built.path.isEmpty())
                result.warnings << QStringLiteral("The timeline's picture could not be built (%1). Export it as one video file "
                                                  "from the editing application and open it in the Video tab.").arg(why);
            else {
                session::VideoReference reference;
                reference.path = built.path.toStdString();
                reference.offset = std::llround(built.start * candidate.sampleRate());
                reference.bundlePath = "media/" + QFileInfo(built.path).fileName().toStdString();
                candidate.video() = reference;
                result.video = QStringLiteral("%1, built from its %2 shots").arg(name.isEmpty() ? QStringLiteral("picture") : name).arg(built.shots);
            }
        } else if (const auto video = object.value("video"); video.isObject()) {
            const auto entry = video.toObject();
            const auto file = entry.value("path").toString();
            require(QFileInfo(file).isAbsolute() && QFileInfo(file).isFile() && file.size() <= 4096,
                    "The AAF's video description is invalid.");
            const auto offset = integer(entry, "offset", -kMaxFrames, kMaxFrames);
            const auto name = entry.value("name").toString().left(256);
            if (!candidate.video().empty())
                result.warnings << QStringLiteral("This session already has a video, so it was kept; the AAF's video %1 was not attached.").arg(name);
            else if (const auto reason = video::videoSupportReason(); !reason.empty())
                result.warnings << QStringLiteral("The AAF's video %1 was not attached: %2").arg(name, QString::fromStdString(reason));
            else {
                session::VideoReference reference;
                reference.path = QFileInfo(file).canonicalFilePath().toStdString();
                reference.offset = offset;
                candidate.video() = reference;
                result.video = name.isEmpty() ? QFileInfo(file).fileName() : name;
            }
        }
        checkCancel(cancel);
        // Drop newly-created channel intermediates after their combined source
        // is complete, while protecting every pre-existing Undo/clipboard source.
        candidate.pruneSources(originalSources);
        result.name=object.value("name").toString();
        result.project=std::move(candidate);
    } catch (const std::exception& e) {
        result.project.reset(); result.error=QString::fromUtf8(e.what());
        // Shared transaction messages name the format actually being imported.
        if (result.format == QLatin1String("OMF")) result.error.replace(QLatin1String("AAF"), QLatin1String("OMF"));
    }
    if (result.format == QLatin1String("OMF"))
        for (auto& warning : result.warnings) warning.replace(QLatin1String("The AAF "), QLatin1String("The OMF "));
    return result;
}
}
