#include "process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spawn.h>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace omawave::io {
namespace {
struct Pipe {
    int fds[2]{-1,-1};
    ~Pipe() { closeRead(); closeWrite(); }
    bool open() {
        if (::pipe2(fds,O_CLOEXEC) != 0) return false;
        return ::fcntl(fds[0],F_SETFL,::fcntl(fds[0],F_GETFL)|O_NONBLOCK) == 0;
    }
    void closeRead() { if(fds[0]>=0){::close(fds[0]);fds[0]=-1;} }
    void closeWrite() { if(fds[1]>=0){::close(fds[1]);fds[1]=-1;} }
};
struct Child {
    pid_t pid = -1;
    ~Child() { stop(); }
    void stop() {
        if(pid<=0)return;
        (void)::kill(-pid,SIGKILL);
        while(::waitpid(pid,nullptr,0)<0 && errno==EINTR) {}
        pid=-1;
    }
};
void retain(std::string& text,std::string_view chunk)
{
    constexpr std::size_t limit=64*1024;
    text.append(chunk);
    if(text.size()>limit)text.erase(0,text.size()-limit);
}
}

std::string findExecutable(const std::string& name)
{
    const auto usable=[](const std::filesystem::path& path) {
        struct stat info{};
        return ::stat(path.c_str(),&info)==0 && S_ISREG(info.st_mode) && ::access(path.c_str(),X_OK)==0;
    };
    if(name.empty())return {};
    if(name.find('/')!=std::string::npos)return usable(name)?std::filesystem::absolute(name).string():std::string{};
    const auto* env=std::getenv("PATH");if(!env)return {};
    const std::string path(env);
    for(std::size_t start=0;start<=path.size();) {
        const auto end=path.find(':',start);const auto entry=path.substr(start,end==std::string::npos?end:end-start);
        const auto candidate=std::filesystem::path(entry.empty()?".":entry)/name;
        if(usable(candidate))return std::filesystem::absolute(candidate).string();
        if(end==std::string::npos)break;
        start=end+1;
    }
    return {};
}

ProcessResult runProcess(const std::vector<std::string>& args,const std::atomic<bool>* cancel,
    std::chrono::milliseconds timeout,const ProcessLine& line,const ProcessChunk& stdoutChunk)
{
    ProcessResult out;
    const auto cancelled=[&]{return cancel && cancel->load(std::memory_order_relaxed);};
    if(cancelled()){out.cancelled=true;return out;}
    if(args.empty() || args[0].empty() || timeout.count()<=0){out.error="invalid process request";return out;}
    for(const auto& arg:args)if(arg.find('\0')!=std::string::npos){out.error="process argument contains a null byte";return out;}
    try {
        Pipe output,diagnostic;
        if(!output.open() || !diagnostic.open()){out.error=std::string("create process pipes: ")+std::strerror(errno);return out;}
        posix_spawn_file_actions_t actions;
        int rc=::posix_spawn_file_actions_init(&actions);
        if(rc){out.error=std::strerror(rc);return out;}
        struct Actions {posix_spawn_file_actions_t* p;~Actions(){::posix_spawn_file_actions_destroy(p);}} actionScope{&actions};
        posix_spawnattr_t attributes;rc=::posix_spawnattr_init(&attributes);
        if(rc){out.error=std::strerror(rc);return out;}
        struct Attributes {posix_spawnattr_t* p;~Attributes(){::posix_spawnattr_destroy(p);}} attributeScope{&attributes};
        rc=::posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
        if(!rc)rc=::posix_spawnattr_setpgroup(&attributes,0);
        if(!rc)rc=::posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
        if(!rc)rc=::posix_spawn_file_actions_adddup2(&actions,output.fds[1],STDOUT_FILENO);
        if(!rc)rc=::posix_spawn_file_actions_adddup2(&actions,diagnostic.fds[1],STDERR_FILENO);
        std::vector<char*> argv;argv.reserve(args.size()+1);
        for(const auto& arg:args)argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        Child child;
        if(!rc)rc=::posix_spawnp(&child.pid,args[0].c_str(),&actions,&attributes,argv.data(),environ);
        if(rc){child.pid=-1;out.error="start "+args[0]+": "+std::strerror(rc);return out;}
        output.closeWrite();diagnostic.closeWrite();
        const auto deadline=std::chrono::steady_clock::now()+timeout;
        std::string pending;bool skipLine=false;
        const auto read=[&](Pipe& pipe,std::string& text,bool parse) {
            // Bound each drain so an output flood cannot postpone cancellation.
            std::array<char,4096> buffer{};
            for(unsigned n=0;n<32 && pipe.fds[0]>=0;++n) {
                const auto count=::read(pipe.fds[0],buffer.data(),buffer.size());
                if(count==0){pipe.closeRead();break;}
                if(count<0){if(errno==EINTR)continue;if(errno!=EAGAIN && errno!=EWOULDBLOCK)pipe.closeRead();break;}
                const std::string_view bytes(buffer.data(),std::size_t(count));retain(text,bytes);
                if(parse && stdoutChunk)stdoutChunk(bytes);
                if(parse && line)for(char c:bytes) {
                    if(c=='\n') {if(!skipLine)line(pending);pending.clear();skipLine=false;}
                    else if(!skipLine) {if(pending.size()<16*1024)pending.push_back(c);else{pending.clear();skipLine=true;}}
                }
            }
        };
        bool finished=false;int status=0;
        while(!finished || output.fds[0]>=0 || diagnostic.fds[0]>=0) {
            if(cancelled()){out.cancelled=true;return out;}
            if(std::chrono::steady_clock::now()>=deadline){out.timedOut=true;out.error="process timed out";return out;}
            read(output,out.output,true);read(diagnostic,out.diagnostic,false);
            if(!finished) {
                siginfo_t info{};
                const auto result=::waitid(P_PID,child.pid,&info,WEXITED|WNOHANG|WNOWAIT);
                if(result==0 && info.si_pid==child.pid)finished=true;
                else if(result<0 && errno!=EINTR){out.error="could not wait for process";return out;}
            }
            if(finished && output.fds[0]<0 && diagnostic.fds[0]<0)break;
            pollfd fds[]{{output.fds[0],POLLIN,0},{diagnostic.fds[0],POLLIN,0}};
            (void)::poll(fds,2,10);
        }
        // Also remove unexpected grandchildren after the direct child exits.
        (void)::kill(-child.pid,SIGKILL);
        while(::waitpid(child.pid,&status,0)<0)if(errno!=EINTR){out.error="could not reap process";return out;}
        child.pid=-1;
        if(line && !skipLine && !pending.empty())line(pending);
        out.exitCode=WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):-1;
        out.ok=out.exitCode==0;
        if(!out.ok)out.error="process exited with status "+std::to_string(out.exitCode);
    } catch(const std::exception& e){out.error=e.what();}
    return out;
}
} // namespace omawave::io
