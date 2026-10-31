#include "image_unpack.h"
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace fs=std::filesystem;
namespace uc::launcher {
namespace {
constexpr std::string_view expected_id="RBUE08";
constexpr int kSpawnFailed=-2;   // wit() could not start Wit at all (no execute bit, a noexec mount): not an exit code
uint64_t folder_size(const fs::path& folder) {
    uint64_t total=0;std::error_code error;
    for(fs::recursive_directory_iterator it(folder,fs::directory_options::skip_permission_denied,error),end;!error && it!=end;it.increment(error)) {
        std::error_code entry_error;
        if(it->is_regular_file(entry_error))total+=it->file_size(entry_error);
    }
    return total;
}
bool inside(const fs::path& file,const fs::path& folder) {
    std::error_code error;
    const auto real=fs::canonical(file,error);   // resolves symlinks that lead into the folder
    for(auto parent=(error ? file : real).parent_path();;parent=parent.parent_path()) {
        if(fs::equivalent(parent,folder,error))return true;
        if(parent==parent.parent_path())return false;
    }
}
}
ImageUnpack::ImageUnpack(fs::path root,fs::path image,fs::path wit,uint64_t required_bytes)
    : root_(std::move(root)),image_(std::move(image)),wit_(wit.empty() ? root_/"tools"/"wit"/"wit" : std::move(wit)),required_bytes_(required_bytes) {
    worker_=std::jthread([this] {run();});
}
ImageUnpack::~ImageUnpack() {cancel();}
void ImageUnpack::cancel() {
    std::lock_guard lock(process_lock_);
    cancelled_=true;
    if(process_)::kill(-static_cast<pid_t>(process_),SIGKILL);   // the group: Wit may have started helpers
}
bool ImageUnpack::stopped() {
    std::lock_guard lock(process_lock_);
    return cancelled_;
}
int ImageUnpack::wit(const std::vector<std::string>& arguments,std::string& output,bool track_progress) {
    int fds[2];
    if(::pipe2(fds,O_CLOEXEC)!=0)return -1;
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_adddup2(&actions,fds[1],1);
    posix_spawn_file_actions_adddup2(&actions,fds[1],2);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attributes,0);   // its own process group
    // The environment of this process plus LC_ALL: with a UTF-8 locale Wit opens paths with Cyrillic and other non-ASCII letters.
    std::vector<std::string> environment;
    for(char** item=environ;item && *item;++item)if(std::string_view(*item).rfind("LC_ALL=",0)!=0)environment.emplace_back(*item);
    environment.emplace_back("LC_ALL=C.UTF-8");
    std::vector<char*> envp;for(auto& item:environment)envp.push_back(item.data());envp.push_back(nullptr);
    std::vector<std::string> command{wit_.string()};command.insert(command.end(),arguments.begin(),arguments.end());
    std::vector<char*> argv;for(auto& item:command)argv.push_back(item.data());argv.push_back(nullptr);
    pid_t child=0;
    const int started=posix_spawn(&child,wit_.c_str(),&actions,&attributes,argv.data(),envp.data());
    posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
    ::close(fds[1]);
    if(started!=0) {::close(fds[0]);return kSpawnFailed;}
    {
        std::lock_guard lock(process_lock_);
        process_=child;
        if(cancelled_)::kill(-child,SIGKILL);
    }
    char buffer[4096];std::string tail;
    for(;;) {
        const auto got=::read(fds[0],buffer,sizeof(buffer));
        if(got<0&&errno==EINTR)continue;
        if(got<=0)break;
        output.append(buffer,static_cast<size_t>(got));
        if(track_progress) {
            tail.append(buffer,static_cast<size_t>(got));if(tail.size()>256)tail.erase(0,tail.size()-256);
            if(auto percent=parse_percent(tail))percent_=std::min(*percent,99);   // 100 is reported once main.dol is checked
        }
    }
    ::close(fds[0]);
    // The child is waited for without being reaped, the group id is forgotten, and only then is it reaped: a cancel() in between signals a zombie's group
    // (harmless), and one after it signals nothing; reaping first would leave a window in which the pid could be reused and cancel() would kill a stranger.
    siginfo_t exited{};
    while(::waitid(P_PID,static_cast<id_t>(child),&exited,WEXITED|WNOWAIT)<0&&errno==EINTR) {}
    {
        std::lock_guard lock(process_lock_);
        process_=0;
    }
    int status=0;
    while(::waitpid(child,&status,0)<0&&errno==EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
void ImageUnpack::run() {
    const auto finish=[&](UnpackStatus status,UnpackError error=UnpackError::none) {error_=error;status_=status;};
    const auto game=root_/"game";   // Wit writes straight into game (no staging folder to rename)
    std::error_code error;
    bool replaced=false;   // the old game folder is touched only after the image was identified
    try {
        if(!fs::is_regular_file(wit_,error))return finish(UnpackStatus::failed,UnpackError::wit_missing);
        if(inside(image_,game))return finish(UnpackStatus::failed,UnpackError::image_in_game_folder);
        std::string id;
        const int identified=wit({"ID6",image_.string()},id,false);
        if(stopped())return finish(UnpackStatus::cancelled);
        if(identified==kSpawnFailed)return finish(UnpackStatus::failed,UnpackError::wit_missing);   // Wit is there but cannot run: not the disc's fault
        while(!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))id.pop_back();
        // Wit prints the error text and exits 0 for a file that is not a disc image, so the ID decides.
        if(identified!=0 || id!=expected_id)return finish(UnpackStatus::failed,UnpackError::wrong_image);
        const auto space=fs::space(root_,error);
        if(!error && space.available+folder_size(game)<required_bytes_)return finish(UnpackStatus::failed,UnpackError::no_space);
        if(stopped())return finish(UnpackStatus::cancelled);   // the walk above can take a while; a cancel must not cost the old game
        replaced=true;
        fs::remove_all(game,error);
        if(error)return finish(UnpackStatus::failed,UnpackError::extract_failed);
        std::string output;
        // --pmode name: without it Wit leaves out the DATA folder when the image holds a single partition (a scrubbed WBFS), and the game is not found.
        const int extracted=wit({"extract",image_.string(),game.string(),"--progress","--pmode","name"},output,true);
        const bool cancelled=stopped();
        std::error_code check_error;
        const bool has_dol=fs::is_regular_file(game/"DATA"/"sys"/"main.dol",check_error);
        if(cancelled || extracted!=0 || !has_dol) {
            if(!cancelled)std::fprintf(stderr,"[unpack] failed: wit exit=%d main.dol=%d\n%s\n",extracted,int(has_dol),output.substr(output.size()>1500 ? output.size()-1500 : 0).c_str());
            fs::remove_all(game,error);
            return cancelled ? finish(UnpackStatus::cancelled) : finish(UnpackStatus::failed,UnpackError::extract_failed);
        }
        percent_=100;
        finish(UnpackStatus::done);
    } catch(...) {
        if(replaced)fs::remove_all(game,error);
        finish(UnpackStatus::failed,UnpackError::extract_failed);
    }
}
}
