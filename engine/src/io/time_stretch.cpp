#include "time_stretch.h"
#include "audio_file.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <spawn.h>
#include <sstream>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace omawave::io {
namespace {
struct Scratch {
    std::string path;
    ~Scratch(){if(!path.empty()){std::error_code ec;std::filesystem::remove_all(path,ec);}}
};
struct Child {
    pid_t pid=-1;
    ~Child(){if(pid>0){(void)::kill(pid,SIGKILL);while(::waitpid(pid,nullptr,0)<0 && errno==EINTR){}}}
};
std::string number(double value){std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<value;return s.str();}
}
StretchResult stretchAudio(const session::AudioBuffer& input,std::uint32_t rate,std::size_t target,const std::atomic<bool>* cancel)
{
    StretchResult out;
    const auto cancelled=[&]{return cancel && cancel->load(std::memory_order_relaxed);};
    try {
        if(cancelled()){out.cancelled=true;return out;}
        const double ratio=input.frames()?static_cast<double>(target)/input.frames():0;
        if(!rate || input.channels()<1 || input.channels()>2 || input.frames()<rate/20 || target<rate/20
            || !std::isfinite(ratio) || ratio<0.25 || ratio>4) {
            out.error="Stretch needs mono/stereo audio of at least 50 ms and a duration between 25% and 400%.";return out;
        }
        const auto validSamples = [&](const session::AudioBuffer& audio) {
            for (unsigned c=0;c<audio.channels();++c) {
                const auto plane=audio.plane(c);
                for(std::size_t i=0;i<plane.size();++i) {
                    if((i&65535)==0) {
                        if(cancelled())return false;
                        if(audio.diskStorage())audio.diskStorage()->releaseMappedPages();
                    }
                    if(!std::isfinite(plane[i]))return false;
                }
            }
            if(audio.diskStorage())audio.diskStorage()->releaseMappedPages();
            return true;
        };
        if(!validSamples(input)) {
            out.cancelled=cancelled();if(!out.cancelled)out.error="Cannot stretch audio containing nonfinite samples.";return out;
        }
        if(target==input.frames()){out.audio=input;return out;}
        std::string pattern=(std::filesystem::temp_directory_path()/"omaaudio-stretch-XXXXXX").string();
        if(!::mkdtemp(pattern.data())){out.error="Cannot create private stretch scratch directory.";return out;}
        Scratch scratch{pattern};const auto in=pattern+"/input.wav",result=pattern+"/output.wav",log=pattern+"/error.log";
        if(auto error=writeFloatWav(in,input,rate);!error.empty()){out.error=error;return out;}
        if(cancelled()){out.cancelled=true;return out;}
        // Pad before the overlap algorithm so its final analysis window has context.
        // Trim in samples after processing: tempo filters can round the final hop.
        std::string filter="apad=pad_len="+std::to_string(rate/2);
        double tempo=1.0/ratio;
        while(tempo<0.5){filter+=",atempo=0.5";tempo/=0.5;}
        while(tempo>2){filter+=",atempo=2";tempo/=2;}
        filter+=",atempo="+number(tempo)+",apad,atrim=end_sample="+std::to_string(target);
        std::vector<std::string> args{"ffmpeg","-nostdin","-hide_banner","-loglevel","error","-y","-i",in,
            "-map","0:a:0","-af",filter,"-ar",std::to_string(rate),"-ac",std::to_string(input.channels()),
            "-c:a","pcm_f32le","-rf64","auto",result};
        std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        int rc=posix_spawn_file_actions_init(&actions);
        if(rc){out.error="Cannot prepare stretch process: "+std::string(std::strerror(rc));return out;}
        struct Actions {posix_spawn_file_actions_t* p;~Actions(){posix_spawn_file_actions_destroy(p);}} cleanup{&actions};
        rc=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
        if(!rc)rc=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
        if(!rc)rc=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,log.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        Child child;
        if(!rc)rc=posix_spawnp(&child.pid,"ffmpeg",&actions,nullptr,argv.data(),environ);
        if(rc){child.pid=-1;out.error="Cannot start FFmpeg for stretching: "+std::string(std::strerror(rc));return out;}
        int status=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(30);
        for(;;){
            if(cancelled()){out.cancelled=true;return out;}
            const auto done=::waitpid(child.pid,&status,WNOHANG);
            if(done==child.pid){child.pid=-1;break;}
            if(done<0 && errno!=EINTR){out.error="Cannot wait for stretch process.";return out;}
            if(std::chrono::steady_clock::now()>deadline){out.error="Stretch process exceeded 30 minutes; no edit applied.";return out;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(!WIFEXITED(status) || WEXITSTATUS(status)!=0){
            char message[4096]{};std::ifstream error(log);error.read(message,sizeof(message)-1);
            out.error="FFmpeg could not stretch this clip: "+std::string(message,static_cast<std::size_t>(error.gcount()));return out;
        }
        if(cancelled()){out.cancelled=true;return out;}
        // Check metadata before allocating a decoded result from an external process.
        const auto info=probeAudioFile(result);
        if(!info.ok || info.frames<0 || static_cast<std::uint64_t>(info.frames)!=target || info.sampleRate!=rate || info.channels!=input.channels()) {
            out.error="Stretch process returned an unexpected duration, rate or channel layout.";return out;
        }
        auto audio=readAudioFile(result);
        if(!audio.ok){out.error=audio.error;return out;}
        if(!validSamples(audio.audio)) {
            out.cancelled=cancelled();if(!out.cancelled)out.error="Stretch process returned invalid audio.";return out;
        }
        if(cancelled()){out.cancelled=true;return out;}
        out.audio=std::move(audio.audio);
    }catch(const std::exception& e){out.error=e.what();}
    return out;
}
}
