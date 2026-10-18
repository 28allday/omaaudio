#include "ffmpeg_audio.h"
#include "audio_file.h"
#include "process.h"
#include <sndfile.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace omawave::io {
namespace {
bool containsEntry(const std::string& text,const std::string& name)
{
    std::istringstream lines(text);std::string line;
    while(std::getline(lines,line)) {
        std::istringstream fields(line);std::string flags,id;fields>>flags>>id;
        if(id==name)return true;
    }
    return false;
}
}
AacSupport aacSupport(const std::atomic<bool>* cancel)
{
    if(cancel && cancel->load(std::memory_order_relaxed))return {false,{},"AAC capability probe cancelled"};
    const auto executable=findExecutable("ffmpeg");
    if(executable.empty())return {false,{},"AAC needs the FFmpeg executable"};
    struct stat info{};
    if(::stat(executable.c_str(),&info)!=0)return {false,executable,"cannot inspect FFmpeg"};
    const auto key=executable+":"+std::to_string(info.st_dev)+":"+std::to_string(info.st_ino)+":"
        +std::to_string(info.st_size)+":"+std::to_string(info.st_mtim.tv_sec)+":"+std::to_string(info.st_mtim.tv_nsec);
    static std::mutex mutex;static std::string previous;static AacSupport cached;
    { const std::lock_guard<std::mutex> lock(mutex); if(key==previous)return cached; }
    // Do not hold the cache mutex across a subprocess: another caller must be
    // able to cancel its own probe independently of this one.
    const auto encoders=runProcess({executable,"-hide_banner","-encoders"},cancel,std::chrono::seconds(5));
    // Only an answer is remembered. A probe that timed out on a busy machine
    // says nothing about FFmpeg, and is asked again next time.
    if(!encoders.ok)return {false,executable,encoders.cancelled?"AAC capability probe cancelled":"FFmpeg did not answer when asked what it supports; try again"};
    const auto muxers=runProcess({executable,"-hide_banner","-muxers"},cancel,std::chrono::seconds(5));
    if(!muxers.ok)return {false,executable,muxers.cancelled?"AAC capability probe cancelled":"FFmpeg did not answer when asked what it supports; try again"};
    const bool available=containsEntry(encoders.output,"aac") && containsEntry(muxers.output,"ipod");
    const std::lock_guard<std::mutex> lock(mutex);
    cached={available,executable,available?"":"this FFmpeg does not provide AAC encoding and the M4A muxer"};
    previous=key;return cached;
}

AacResult encodeAac(const session::AudioBuffer& audio,std::uint32_t rate,unsigned bitrateKbps,
    const std::string& output,const std::atomic<bool>* cancel,const std::function<void(float)>& progress)
{
    AacResult out;
    const auto cancelled=[&]{return cancel && cancel->load(std::memory_order_relaxed);};
    if(cancelled()){out.cancelled=true;return out;}
    try {
        const auto support=aacSupport(cancel);
        if(!support.available){out.cancelled=cancelled();out.error=out.cancelled?"AAC export cancelled":support.reason;return out;}
        const auto target=std::filesystem::absolute(output);
        auto pattern=(target.parent_path()/"aac-input-XXXXXX").string();
        const int inputFd=::mkstemp(pattern.data());
        if(inputFd<0){out.error="cannot create AAC input staging file";return out;}
        ::close(inputFd);const std::filesystem::path input(pattern);
        struct Input {std::filesystem::path path;~Input(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{input};
        if(auto error=writeFloatWav(input.string(),audio,rate,cancel);!error.empty()) {
            out.cancelled=cancelled();out.error=std::move(error);return out;
        }
        if(progress)progress(0.15f);
        const auto encoded=runProcess({support.executable,"-nostdin","-hide_banner","-loglevel","error","-nostats","-y",
            "-i",input.string(),"-map","0:a:0","-vn","-c:a","aac","-profile:a","aac_low",
            "-b:a",std::to_string(bitrateKbps)+"k","-ar",std::to_string(rate),"-ac",std::to_string(audio.channels()),
            "-movflags","+faststart","-progress","pipe:1","-f","ipod",target.string()},cancel,std::chrono::minutes(30),
            [&](std::string_view line) {
                constexpr std::string_view prefix="out_time_us=";
                if(!progress || !line.starts_with(prefix))return;
                std::int64_t time=0;const auto value=line.substr(prefix.size());
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),time);
                if(parsed.ec==std::errc{} && time>=0 && audio.frames()>0)
                    progress(0.15f+0.84f*std::clamp(float(double(time)*rate/(1e6*audio.frames())),0.0f,1.0f));
            });
        out.cancelled=encoded.cancelled || cancelled();
        if(!encoded.ok || out.cancelled) {
            out.error=out.cancelled?"AAC export cancelled":"AAC encoding failed: "+encoded.error+" "+encoded.diagnostic;
            return out;
        }
        // Check the staged container before the caller installs it.
        char header[12]{};std::ifstream file(target,std::ios::binary);file.read(header,sizeof(header));
        if(file.gcount()!=sizeof(header) || std::string_view(header+4,4)!="ftyp" || std::filesystem::file_size(target)<32) {
            out.error="AAC encoder returned an invalid M4A container";return out;
        }
        out.ok=true;if(progress)progress(1);
    } catch(const std::exception& e){out.ok=false;out.error=e.what();}
    return out;
}

namespace {
std::string lowerExtension(const std::string& path)
{
    const auto ext=std::filesystem::path(path).extension().string();
    std::string out;for(const char c:ext)out+=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::filesystem::path stagingDirectory()
{
    std::filesystem::path base;
    if(const char* cachePath=std::getenv("XDG_CACHE_HOME");cachePath && *cachePath && cachePath[0]=='/')base=cachePath;
    else if(const char* home=std::getenv("HOME");home && *home)base=std::filesystem::path(home)/".cache";
    else throw std::runtime_error("no user cache directory is available for decoding");
    const auto path=base/"omaaudio"/"audio-scratch";
    std::filesystem::create_directories(path);
    return path;
}
// A decode killed with the application leaves its staged WAV behind, gigabytes
// of it for a long file. One untouched for an hour belongs to nobody: a decode
// still running writes to its file continuously and is stopped at thirty minutes.
void removeAbandonedDecodes(const std::filesystem::path& staging)
{
    std::error_code ec;
    const auto now=std::filesystem::file_time_type::clock::now();
    for(std::filesystem::directory_iterator it(staging,ec),end;!ec && it!=end;it.increment(ec)) {
        const auto name=it->path().filename().string();
        if(!name.starts_with("decode-") || !name.ends_with(".wav") || !it->is_regular_file(ec))continue;
        const auto written=std::filesystem::last_write_time(it->path(),ec);
        if(!ec && now-written>std::chrono::hours(1))std::filesystem::remove(it->path(),ec);
        ec.clear();
    }
}
// "key=value" lines from ffprobe's default writer.
std::string fieldOf(const std::string& text,const std::string& key)
{
    std::istringstream lines(text);std::string line;
    while(std::getline(lines,line)) {
        if(line.starts_with(key+"=")) return line.substr(key.size()+1);
    }
    return {};
}

bool positiveInteger(const std::string& text,std::uint32_t& out)
{
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),out);
    return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && out>0;
}

// Own the path before creating the file, so even an exception while publishing
// its name cannot strand it. A successful caller takes ownership explicitly.
struct DecodeScratch {
    std::string path;
    bool created=false,keep=false;
    ~DecodeScratch() { if(created && !keep) {std::error_code ec;std::filesystem::remove(path,ec);} }
};
}

bool isCompressedAudioPath(const std::string& path)
{
    const auto ext=lowerExtension(path);
    // MPEG-4 audio, and the audio in MXF (Avid OP-Atom and camera media) and
    // QuickTime files that editing applications reference from AAF and OMF.
    return ext==".m4a" || ext==".aac" || ext==".mp4" || ext==".m4b" || ext==".mxf" || ext==".mov" || ext==".qt";
}

DecodeSupport aacDecodeSupport(const std::atomic<bool>* cancel)
{
    if(cancel && cancel->load(std::memory_order_relaxed))return {false,{},{},"AAC capability probe cancelled"};
    const auto executable=findExecutable("ffmpeg");
    const auto probe=findExecutable("ffprobe");
    if(executable.empty())return {false,{},{},"AAC/M4A files need the FFmpeg executable (package ffmpeg)"};
    if(probe.empty())return {false,executable,{},"AAC/M4A files need ffprobe, which ships with FFmpeg"};
    struct stat info{};
    if(::stat(executable.c_str(),&info)!=0)return {false,executable,probe,"cannot inspect FFmpeg"};
    const auto key=executable+":"+std::to_string(info.st_dev)+":"+std::to_string(info.st_ino)+":"
        +std::to_string(info.st_size)+":"+std::to_string(info.st_mtim.tv_sec)+":"+std::to_string(info.st_mtim.tv_nsec);
    static std::mutex mutex;static std::string previous;static DecodeSupport cached;
    { const std::lock_guard<std::mutex> lock(mutex); if(key==previous)return cached; }
    const auto decoders=runProcess({executable,"-hide_banner","-decoders"},cancel,std::chrono::seconds(5));
    if(!decoders.ok)return {false,executable,probe,decoders.cancelled?"AAC capability probe cancelled":"FFmpeg did not answer when asked what it supports; try again"};
    const auto demuxers=runProcess({executable,"-hide_banner","-demuxers"},cancel,std::chrono::seconds(5));
    if(!demuxers.ok)return {false,executable,probe,demuxers.cancelled?"AAC capability probe cancelled":"FFmpeg did not answer when asked what it supports; try again"};
    const bool available=containsEntry(decoders.output,"aac")
        && (containsEntry(demuxers.output,"mov,mp4,m4a,3gp,3g2,mj2") || demuxers.output.find("mov,mp4,m4a")!=std::string::npos);
    const std::lock_guard<std::mutex> lock(mutex);
    cached={available,executable,probe,available?"":"this FFmpeg cannot decode AAC in an MPEG-4 container"};
    previous=key;return cached;
}

DecodedInfo probeCompressedAudio(const std::string& path,const std::atomic<bool>* cancel)
{
    DecodedInfo out;
    const auto support=aacDecodeSupport(cancel);
    if(!support.available){out.error=support.reason;return out;}
    const auto probed=runProcess({support.probeExecutable,"-v","error","-select_streams","a:0",
        "-show_entries","stream=codec_name,sample_rate,channels:format=duration","-of","default=noprint_wrappers=1",
        path},cancel,std::chrono::seconds(20));
    if(probed.cancelled){out.error="audio probe cancelled";return out;}
    if(!probed.ok){out.error="not a readable audio file: "+(probed.diagnostic.empty()?probed.error:probed.diagnostic);return out;}
    out.codec=fieldOf(probed.output,"codec_name");
    const auto rate=fieldOf(probed.output,"sample_rate");const auto channels=fieldOf(probed.output,"channels");
    const auto duration=fieldOf(probed.output,"duration");
    if(out.codec.empty() || rate.empty() || channels.empty()){out.error="the file has no audio stream FFmpeg can read";return out;}
    if(!positiveInteger(rate,out.sampleRate) || !positiveInteger(channels,out.channels)) {
        out.error="the audio stream reports an invalid sample rate or channel count";return out;
    }
    if(!duration.empty() && duration!="N/A") {
        const auto parsed=std::from_chars(duration.data(),duration.data()+duration.size(),out.seconds);
        if(parsed.ec!=std::errc{} || parsed.ptr!=duration.data()+duration.size()
            || !std::isfinite(out.seconds) || out.seconds<0) {
            out.error="the file reports an invalid audio duration";return out;
        }
    }
    const auto frames=std::round(static_cast<long double>(out.seconds)*out.sampleRate);
    if(frames>=9223372036854775808.0L) {out.error="the audio duration exceeds the supported frame range";return out;}
    out.frames=static_cast<std::int64_t>(frames);
    out.ok=true;return out;
}

DecodedAudio decodeCompressedAudio(const std::string& path,const std::atomic<bool>* cancel,
    const std::function<void(float)>& progress)
{
    DecodedAudio out;
    const auto cancelled=[&]{return cancel && cancel->load(std::memory_order_relaxed);};
    if(cancelled()){out.cancelled=true;out.error="import cancelled";return out;}
    try {
        const auto support=aacDecodeSupport(cancel);
        if(!support.available){out.cancelled=cancelled();out.error=out.cancelled?"import cancelled":support.reason;return out;}
        const auto info=probeCompressedAudio(path,cancel);
        if(!info.ok || cancelled()){out.cancelled=cancelled();out.error=out.cancelled?"import cancelled":info.error;return out;}
        out.codec=info.codec;out.sampleRate=info.sampleRate;out.channels=info.channels;
        if(info.channels>8){out.error="the file has "+std::to_string(info.channels)+" channels; up to 8 can be imported";return out;}
        if(info.seconds>6*3600.0){out.error="the file is longer than six hours; split it before importing";return out;}
        out.declaredFrames=info.frames;
        const auto staging=stagingDirectory();
        removeAbandonedDecodes(staging);
        // Six hours, uint32 rate and at most eight channels fit comfortably in
        // uint64. Validate those bounds before converting or multiplying.
        const auto estimate=static_cast<std::uintmax_t>(std::ceil(info.seconds*info.sampleRate))*info.channels*sizeof(float)+1024;
        std::error_code ec;const auto space=std::filesystem::space(staging,ec);
        if(!ec && space.available<estimate+(std::uintmax_t(512)<<20)) {
            out.error="not enough free space in "+staging.string()+" to decode this file ("+std::to_string(estimate>>20)+" MiB needed)";
            return out;
        }
        DecodeScratch scratch{(staging/"decode-XXXXXX.wav").string()};
        const int fd=::mkstemps(scratch.path.data(),4);
        if(fd<0){out.error="cannot create a decoding file in "+staging.string();return out;}
        scratch.created=true;
        ::close(fd);
        out.stagedWav=scratch.path;
        if(progress)progress(0.05f);
        const double total=info.seconds>0?info.seconds:1.0;
        const auto decoded=runProcess({support.executable,"-nostdin","-hide_banner","-loglevel","error","-nostats","-y",
            "-i",path,"-map","0:a:0","-vn","-c:a","pcm_f32le","-rf64","auto","-f","wav","-progress","pipe:1",out.stagedWav},
            cancel,std::chrono::minutes(30),
            [&](std::string_view line) {
                constexpr std::string_view prefix="out_time_us=";
                if(!progress || !line.starts_with(prefix))return;
                std::int64_t time=0;const auto value=line.substr(prefix.size());
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),time);
                if(parsed.ec==std::errc{} && time>=0)
                    progress(0.05f+0.9f*std::clamp(float(double(time)/(1e6*total)),0.0f,1.0f));
            });
        out.cancelled=decoded.cancelled || cancelled();
        if(!decoded.ok || out.cancelled) {
            out.stagedWav.clear();
            out.error=out.cancelled?"import cancelled":"FFmpeg could not decode the file: "
                +(decoded.diagnostic.empty()?decoded.error:decoded.diagnostic);
            return out;
        }
        if(std::filesystem::file_size(out.stagedWav,ec)<=44 || ec) {
            out.stagedWav.clear();
            out.error="FFmpeg decoded no audio from the file";return out;
        }
        // A container can omit its duration or report a misleading one. Check
        // the actual decoded header before the caller allocates or copies audio.
        SF_INFO actual{};
        if(auto* file=sf_open(out.stagedWav.c_str(),SFM_READ,&actual))sf_close(file);
        else {
            out.stagedWav.clear();out.error="FFmpeg did not produce readable decoded audio";return out;
        }
        if(actual.frames<=0 || actual.samplerate<=0 || actual.channels<=0 || actual.channels>8) {
            out.stagedWav.clear();out.error="FFmpeg decoded no usable audio (up to 8 channels are supported)";return out;
        }
        if(actual.frames>std::int64_t(actual.samplerate)*21600) {
            out.stagedWav.clear();out.error="the decoded file is longer than six hours; split it before importing";return out;
        }
        if(!decoded.diagnostic.empty())
            out.warning="FFmpeg reported problems while decoding; the file may be damaged: "+decoded.diagnostic;
        out.ok=true;if(progress)progress(0.95f);
        scratch.keep=true;
    } catch(const std::exception& e){out.ok=false;out.stagedWav.clear();out.error=e.what();}
    return out;
}
} // namespace omawave::io
